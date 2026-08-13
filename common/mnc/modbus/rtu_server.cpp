#include "mnc/modbus/rtu_server.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/serial_port.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/crc.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace mnc::modbus {
namespace {

using boost::asio::awaitable;
using boost::asio::use_awaitable;

constexpr std::size_t maximum_rtu_frame_size = 256;
constexpr std::size_t maximum_queued_rtu_responses = 64;
constexpr std::size_t maximum_queued_rtu_bytes =
	maximum_queued_rtu_responses * maximum_rtu_frame_size;

std::uint8_t octet(std::byte value)
{
	return std::to_integer<std::uint8_t>(value);
}

void report(const ErrorHandler &handler, std::string_view transport,
	std::string message)
{
	if (handler)
		handler(transport, message);
}

} // namespace

std::uint16_t crc16(std::span<const std::byte> bytes)
{
	using ModbusCrc = boost::crc_optimal<16, 0x8005, 0xffff, 0x0000,
		true, true>;
	ModbusCrc crc;
	crc.process_bytes(bytes.data(), bytes.size());
	return crc.checksum();
}

bool valid_crc(std::span<const std::byte> frame)
{
	if (frame.size() < 4)
		return false;
	const auto expected = crc16(frame.first(frame.size() - 2));
	return octet(frame[frame.size() - 2]) == (expected & 0xffu) &&
	       octet(frame[frame.size() - 1]) == (expected >> 8u);
}

std::optional<std::size_t> RtuFrameAssembler::frame_size(
	std::span<const std::byte> bytes)
{
	if (bytes.size() < 2)
		return std::nullopt;
	const auto function = octet(bytes[1]);
	switch (static_cast<FunctionCode>(function)) {
	case FunctionCode::read_holding_registers:
	case FunctionCode::read_input_registers:
	case FunctionCode::write_single_register:
		return 8;
	case FunctionCode::write_multiple_registers:
		if (bytes.size() < 7)
			return std::nullopt;
		return 9 + octet(bytes[6]);
	default:
		/* Unsupported functions have no generic request length. The t3.5
		 * silent interval, not an arbitrary eight-byte assumption, delimits
		 * the complete frame. */
		return std::nullopt;
	}
}

std::vector<std::vector<std::byte>> RtuFrameAssembler::push(
	std::span<const std::byte> bytes)
{
	buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
	std::vector<std::vector<std::byte>> frames;
	for (;;) {
		const auto size = frame_size(buffer_);
		if (!size) {
			if (buffer_.size() > maximum_rtu_frame_size) {
				buffer_.clear();
				throw std::length_error(
					"RTU frame exceeded its bound");
			}
			break;
		}
		if (*size > maximum_rtu_frame_size) {
			buffer_.clear();
			throw std::length_error("RTU frame exceeded its bound");
		}
		if (buffer_.size() < *size)
			break;
		frames.emplace_back(buffer_.begin(), buffer_.begin() + *size);
		buffer_.erase(buffer_.begin(), buffer_.begin() + *size);
	}
	return frames;
}

std::optional<std::vector<std::byte>>
RtuFrameAssembler::finish_on_silence()
{
	if (buffer_.empty())
		return std::nullopt;
	std::vector<std::byte> frame;
	frame.swap(buffer_);
	return frame;
}

struct ModbusRtuServer::Impl {
	struct Port : std::enable_shared_from_this<Port> {
		Port(boost::asio::any_io_executor executor, RequestHandler &handler,
		     RtuPortConfig config, ErrorHandler errors,
		     std::shared_ptr<std::atomic<bool>> running)
			: serial(executor), gap_timer(std::move(executor)),
			  handler(handler), config(std::move(config)),
			  errors(std::move(errors)), running(std::move(running)),
			  gap_duration(character_gap(this->config))
		{
		}

		static std::chrono::microseconds character_gap(
			const RtuPortConfig &config)
		{
			if (config.baud_rate > 19'200)
				return std::chrono::microseconds(1'750);
			const std::uint32_t parity_bits =
				config.parity == SerialParity::none ? 0u : 1u;
			const std::uint32_t bits_per_character =
				1u + config.data_bits + parity_bits + config.stop_bits;
			const std::uint64_t numerator =
				3'500'000ull * bits_per_character;
			return std::chrono::microseconds(
				(numerator + config.baud_rate - 1) /
				config.baud_rate);
		}

		void open()
		{
			serial.open(config.device);
			serial.set_option(boost::asio::serial_port_base::baud_rate(
				config.baud_rate));
			serial.set_option(
				boost::asio::serial_port_base::character_size(
					config.data_bits));
			using Parity = boost::asio::serial_port_base::parity;
			const auto parity = config.parity == SerialParity::none
				? Parity::none
				: config.parity == SerialParity::even
					? Parity::even
					: Parity::odd;
			serial.set_option(Parity(parity));
			using Stop = boost::asio::serial_port_base::stop_bits;
			serial.set_option(Stop(config.stop_bits == 2
				? Stop::two : Stop::one));
			active = true;
		}

		void close() noexcept
		{
			if (!active.exchange(false) && !serial.is_open())
				return;
			++gap_generation;
			boost::system::error_code ignored;
			gap_timer.cancel(ignored);
			serial.cancel(ignored);
			serial.close(ignored);
			{
				std::scoped_lock lock(assembler_mutex);
				assembler.clear();
			}
			{
				std::scoped_lock lock(output_mutex);
				output_queue.clear();
				queued_output_bytes = 0;
				writing = false;
			}
		}

		void process_frame(std::vector<std::byte> frame)
		{
			if (!valid_crc(frame)) {
				report(errors, "rtu", config.device +
					": CRC mismatch or truncated RTU frame");
				return;
			}
			const auto unit = octet(frame[0]);
			auto response = handler.handle(
				Request{unit, std::span(frame).subspan(
					1, frame.size() - 3), true},
				config.unit_id);
			if (!response)
				return;

			std::vector<std::byte> output;
			output.reserve(response->pdu.size() + 3);
			output.push_back(static_cast<std::byte>(response->unit_id));
			output.insert(output.end(), response->pdu.begin(),
				response->pdu.end());
			const auto checksum = crc16(output);
			output.push_back(static_cast<std::byte>(checksum & 0xffu));
			output.push_back(static_cast<std::byte>(checksum >> 8u));
			enqueue_write(std::move(output));
		}

		void enqueue_write(std::vector<std::byte> output)
		{
			bool start_writer = false;
			bool queue_full = false;
			{
				std::scoped_lock lock(output_mutex);
				if (!active)
					return;
				if (output_queue.size() >= maximum_queued_rtu_responses ||
				    queued_output_bytes + output.size() >
					maximum_queued_rtu_bytes) {
					queue_full = true;
				} else {
					queued_output_bytes += output.size();
					output_queue.push_back(std::move(output));
				}
				if (!queue_full && !writing) {
					writing = true;
					start_writer = true;
				}
			}
			if (queue_full) {
				report(errors, "rtu", config.device +
					": response queue limit exceeded; response dropped");
				return;
			}
			if (!start_writer)
				return;

			auto self = shared_from_this();
			boost::asio::co_spawn(serial.get_executor(),
				[self]() -> awaitable<void> {
					co_await self->drain_writes();
				}, boost::asio::detached);
		}

		awaitable<void> drain_writes()
		{
			try {
				while (*running && active) {
					std::vector<std::byte> output;
					{
						std::scoped_lock lock(output_mutex);
						if (output_queue.empty()) {
							writing = false;
							co_return;
						}
						output = output_queue.front();
					}
					co_await boost::asio::async_write(serial,
						boost::asio::buffer(output), use_awaitable);
					{
						std::scoped_lock lock(output_mutex);
						/* stop() may have cleared the queue while the write
						 * completion was pending. */
						if (!output_queue.empty()) {
							queued_output_bytes -=
								output_queue.front().size();
							output_queue.pop_front();
						}
					}
				}
			} catch (const std::exception &error) {
				{
					std::scoped_lock lock(output_mutex);
					output_queue.clear();
					writing = false;
				}
				if (*running && active)
					report(errors, "rtu", config.device + ": " +
						error.what());
				close();
			}
		}

		void arm_gap_timer()
		{
			if (!active)
				return;
			const auto generation = ++gap_generation;
			gap_timer.expires_after(gap_duration);
			auto self = shared_from_this();
			gap_timer.async_wait(
				[self, generation](const boost::system::error_code &error) {
					if (error == boost::asio::error::operation_aborted ||
					    !*self->running || !self->active ||
					    generation != self->gap_generation.load())
						return;
					std::optional<std::vector<std::byte>> frame;
					{
						std::scoped_lock lock(self->assembler_mutex);
						frame = self->assembler.finish_on_silence();
					}
					if (frame)
						self->process_frame(std::move(*frame));
				});
		}

		awaitable<void> run()
		{
			try {
				while (*running && active) {
					const auto count = co_await serial.async_read_some(
						boost::asio::buffer(input), use_awaitable);
					boost::system::error_code ignored;
					gap_timer.cancel(ignored);
					std::vector<std::vector<std::byte>> frames;
					try {
						std::scoped_lock lock(assembler_mutex);
						frames = assembler.push(
							std::span(input).first(count));
					} catch (const std::length_error &error) {
						report(errors, "rtu", config.device + ": " +
							error.what());
					}
					arm_gap_timer();
					for (auto &frame : frames)
						process_frame(std::move(frame));
				}
			} catch (const std::exception &error) {
				if (*running && active)
					report(errors, "rtu", config.device + ": " +
						error.what());
				close();
			}
		}

		boost::asio::serial_port serial;
		boost::asio::steady_timer gap_timer;
		RequestHandler &handler;
		RtuPortConfig config;
		ErrorHandler errors;
		std::shared_ptr<std::atomic<bool>> running;
		RtuFrameAssembler assembler;
		std::mutex assembler_mutex;
		std::mutex output_mutex;
		std::deque<std::vector<std::byte>> output_queue;
		std::size_t queued_output_bytes = 0;
		bool writing = false;
		std::atomic<bool> active{false};
		std::atomic<std::uint64_t> gap_generation{0};
		std::chrono::microseconds gap_duration;
		std::array<std::byte, maximum_rtu_frame_size> input{};
	};

	Impl(boost::asio::any_io_executor executor, RequestHandler &handler,
	     std::vector<RtuPortConfig> configs, ErrorHandler errors)
		: executor(std::move(executor)), handler(handler),
		  configs(std::move(configs)), errors(std::move(errors))
	{
	}

	boost::asio::any_io_executor executor;
	RequestHandler &handler;
	std::vector<RtuPortConfig> configs;
	ErrorHandler errors;
	std::vector<std::shared_ptr<Port>> ports;
	std::shared_ptr<std::atomic<bool>> running =
		std::make_shared<std::atomic<bool>>(false);
};

ModbusRtuServer::ModbusRtuServer(boost::asio::any_io_executor executor,
	RequestHandler &handler, std::vector<RtuPortConfig> ports,
	ErrorHandler errors)
	: impl_(std::make_shared<Impl>(std::move(executor), handler,
		std::move(ports), std::move(errors)))
{
}

ModbusRtuServer::~ModbusRtuServer()
{
	stop();
}

void ModbusRtuServer::start()
{
	if (impl_->running->exchange(true))
		return;

	std::vector<std::shared_ptr<Impl::Port>> opened;
	try {
		if (impl_->configs.empty())
			throw std::invalid_argument(
				"Modbus RTU requires at least one serial port");
		opened.reserve(impl_->configs.size());
		for (const auto &config : impl_->configs) {
			if (config.unit_id == 0 || config.unit_id > 247)
				throw std::invalid_argument(
					"Modbus RTU unit id must be 1..247");
			if (config.device.empty())
				throw std::invalid_argument(
					"Modbus RTU device path is empty");
			auto port = std::make_shared<Impl::Port>(impl_->executor,
				impl_->handler, config, impl_->errors, impl_->running);
			try {
				port->open();
			} catch (const std::exception &error) {
				throw std::runtime_error("failed to open " + config.device +
					": " + error.what());
			}
			opened.push_back(std::move(port));
		}

		impl_->ports = std::move(opened);
		for (const auto &port : impl_->ports) {
			boost::asio::co_spawn(impl_->executor,
				[port]() -> awaitable<void> {
					co_await port->run();
				}, boost::asio::detached);
		}
	} catch (...) {
		for (const auto &port : opened) {
			port->close();
		}
		/* The vector has already moved into impl_->ports when coroutine
		 * launch fails. Close that ownership path as well so a rejected
		 * runtime configuration cannot leave a serial device reserved. */
		for (const auto &port : impl_->ports) {
			port->close();
		}
		impl_->ports.clear();
		impl_->running->store(false);
		throw;
	}
}

void ModbusRtuServer::stop() noexcept
{
	if (!impl_ || !impl_->running->exchange(false))
		return;
	for (const auto &port : impl_->ports)
		port->close();
	impl_->ports.clear();
}

} // namespace mnc::modbus
