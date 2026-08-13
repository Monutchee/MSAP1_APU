#include "mnc/modbus/modbus.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/serial_port.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/crc.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <utility>

namespace mnc::modbus {
namespace {

using boost::asio::awaitable;
using boost::asio::use_awaitable;

constexpr std::size_t tcp_header_size = 7;
constexpr std::size_t maximum_pdu_size = 253;
constexpr std::size_t maximum_rtu_frame_size = 256;

std::uint8_t byte(std::byte value)
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

void ModbusServer::add(std::unique_ptr<ModbusTransport> transport)
{
	if (!transport)
		throw std::invalid_argument("Modbus transport is null");
	if (started_ != 0)
		throw std::logic_error("cannot add a running Modbus transport");
	transports_.push_back(std::move(transport));
}

void ModbusServer::start()
{
	if (started_ != 0)
		return;
	try {
		for (auto &transport : transports_) {
			transport->start();
			++started_;
		}
	} catch (...) {
		stop();
		throw;
	}
}

void ModbusServer::stop() noexcept
{
	while (started_ != 0) {
		--started_;
		transports_[started_]->stop();
	}
}

std::uint16_t read_u16_be(std::span<const std::byte> bytes)
{
	if (bytes.size() < 2)
		throw std::invalid_argument("truncated 16-bit Modbus value");
	return static_cast<std::uint16_t>((byte(bytes[0]) << 8u) | byte(bytes[1]));
}

void append_u16_be(std::vector<std::byte> &bytes, std::uint16_t value)
{
	bytes.push_back(static_cast<std::byte>((value >> 8u) & 0xffu));
	bytes.push_back(static_cast<std::byte>(value & 0xffu));
}

std::vector<std::uint16_t> encode_u32(std::uint32_t value)
{
	return {static_cast<std::uint16_t>(value >> 16u),
		static_cast<std::uint16_t>(value & 0xffffu)};
}

std::vector<std::uint16_t> encode_u16(std::uint16_t value)
{
	return {value};
}

std::vector<std::uint16_t> encode_u64(std::uint64_t value)
{
	return {static_cast<std::uint16_t>(value >> 48u),
		static_cast<std::uint16_t>((value >> 32u) & 0xffffu),
		static_cast<std::uint16_t>((value >> 16u) & 0xffffu),
		static_cast<std::uint16_t>(value & 0xffffu)};
}

std::vector<std::uint16_t> encode_float(float value)
{
	return encode_u32(std::bit_cast<std::uint32_t>(value));
}

std::vector<std::uint16_t> encode_i32(std::int32_t value)
{
	return encode_u32(std::bit_cast<std::uint32_t>(value));
}

std::vector<std::byte> RequestHandler::exception(
	std::uint8_t function, ExceptionCode code) const
{
	return {static_cast<std::byte>(function | 0x80u),
		static_cast<std::byte>(code)};
}

std::optional<Response> RequestHandler::handle(
	const Request &request, std::uint8_t configured_unit_id)
{
	const auto pdu = request.pdu;
	if (configured_unit_id == 0 || configured_unit_id > 247)
		throw std::invalid_argument("Modbus unit id must be 1..247");
	auto response = [&](std::vector<std::byte> value) {
		return std::optional<Response>{Response{request.unit_id,
			std::move(value)}};
	};
	if (pdu.empty())
		return response(exception(0, ExceptionCode::illegal_function));
	const auto function = byte(pdu[0]);
	const bool broadcast = request.broadcast_allowed && request.unit_id == 0;
	if (!broadcast && request.unit_id != configured_unit_id)
		return std::nullopt;

	auto require_size = [&](std::size_t size) {
		return pdu.size() == size;
	};
	auto address = [&] { return read_u16_be(pdu.subspan(1, 2)); };
	auto quantity = [&] { return read_u16_be(pdu.subspan(3, 2)); };

	switch (static_cast<FunctionCode>(function)) {
	case FunctionCode::read_holding_registers:
	case FunctionCode::read_input_registers: {
		if (broadcast)
			return std::nullopt;
		if (!require_size(5))
			return response(exception(function, ExceptionCode::illegal_data_value));
		const auto count = quantity();
		if (count == 0 || count > 125)
			return response(exception(function, ExceptionCode::illegal_data_value));
		const auto table = function ==
			static_cast<std::uint8_t>(FunctionCode::read_holding_registers)
			? RegisterTable::holding : RegisterTable::input;
		auto result = registers_.read(table, address(), count);
		if (result.exception != ExceptionCode::none)
			return response(exception(function, result.exception));
		if (result.values.size() != count)
			return response(exception(function,
				ExceptionCode::server_device_failure));
		std::vector<std::byte> response;
		response.reserve(2 + result.values.size() * 2);
		response.push_back(static_cast<std::byte>(function));
		response.push_back(static_cast<std::byte>(result.values.size() * 2));
		for (const auto value : result.values)
			append_u16_be(response, value);
		return Response{request.unit_id, std::move(response)};
	}
	case FunctionCode::write_single_register: {
		if (!require_size(5)) {
			if (broadcast)
				return std::nullopt;
			return response(exception(function, ExceptionCode::illegal_data_value));
		}
		const auto code = registers_.write_single(address(), quantity());
		if (broadcast)
			return std::nullopt;
		if (code != ExceptionCode::none)
			return response(exception(function, code));
		return Response{request.unit_id,
			std::vector<std::byte>(pdu.begin(), pdu.end())};
	}
	case FunctionCode::write_multiple_registers: {
		if (pdu.size() < 6) {
			if (broadcast)
				return std::nullopt;
			return response(exception(function, ExceptionCode::illegal_data_value));
		}
		const auto count = quantity();
		const auto byte_count = byte(pdu[5]);
		if (count == 0 || count > 123 || byte_count != count * 2 ||
		    pdu.size() != static_cast<std::size_t>(6 + byte_count)) {
			if (broadcast)
				return std::nullopt;
			return response(exception(function, ExceptionCode::illegal_data_value));
		}
		std::vector<std::uint16_t> values;
		values.reserve(count);
		for (std::uint16_t index = 0; index < count; ++index)
			values.push_back(read_u16_be(pdu.subspan(6 + index * 2, 2)));
		const auto code = registers_.write_multiple(address(), values);
		if (broadcast)
			return std::nullopt;
		if (code != ExceptionCode::none)
			return response(exception(function, code));
		std::vector<std::byte> response{static_cast<std::byte>(function)};
		append_u16_be(response, address());
		append_u16_be(response, count);
		return Response{request.unit_id, std::move(response)};
	}
	default:
		if (broadcast)
			return std::nullopt;
		return response(exception(function, ExceptionCode::illegal_function));
	}
}

struct ModbusTcpServer::Impl : std::enable_shared_from_this<Impl> {
	Impl(boost::asio::any_io_executor executor, RequestHandler &handler,
	     TcpServerConfig config, ErrorHandler errors)
		: executor(std::move(executor)), acceptor(this->executor),
		  handler(handler), config(std::move(config)),
		  errors(std::move(errors))
	{
	}

	awaitable<void> session(std::shared_ptr<boost::asio::ip::tcp::socket> socket)
	{
		++clients;
		struct Guard {
			std::atomic<std::uint32_t> &count;
			~Guard() { --count; }
		} guard{clients};
		try {
			for (;;) {
				std::array<std::byte, tcp_header_size> header{};
				co_await boost::asio::async_read(*socket,
					boost::asio::buffer(header), use_awaitable);
				const auto transaction = read_u16_be(header);
				const auto protocol = read_u16_be(
					std::span(header).subspan(2, 2));
				const auto length = read_u16_be(
					std::span(header).subspan(4, 2));
				if (protocol != 0 || length < 2 ||
				    length > maximum_pdu_size + 1)
					throw std::runtime_error("invalid MBAP header");
				const auto unit = byte(header[6]);
				std::vector<std::byte> pdu(length - 1);
				co_await boost::asio::async_read(*socket,
					boost::asio::buffer(pdu), use_awaitable);
				auto response = handler.handle(
					Request{unit, pdu, false}, config.unit_id);
				if (!response)
					continue;
				std::vector<std::byte> adu;
				adu.reserve(tcp_header_size + response->pdu.size());
				append_u16_be(adu, transaction);
				append_u16_be(adu, 0);
				append_u16_be(adu,
					static_cast<std::uint16_t>(response->pdu.size() + 1));
				adu.push_back(static_cast<std::byte>(response->unit_id));
				adu.insert(adu.end(), response->pdu.begin(), response->pdu.end());
				co_await boost::asio::async_write(*socket,
					boost::asio::buffer(adu), use_awaitable);
			}
		} catch (const std::exception &error) {
			if (running)
				report(errors, "tcp", error.what());
		}
		boost::system::error_code ignored;
		socket->close(ignored);
		std::lock_guard lock(sockets_mutex);
		sockets.erase(socket);
	}

	awaitable<void> accept_loop()
	{
		while (running) {
			try {
				auto socket = std::make_shared<boost::asio::ip::tcp::socket>(executor);
				co_await acceptor.async_accept(*socket, use_awaitable);
				if (clients >= config.maximum_clients) {
					boost::system::error_code ignored;
					socket->close(ignored);
					continue;
				}
				{
					std::lock_guard lock(sockets_mutex);
					sockets.insert(socket);
				}
				auto self = shared_from_this();
				boost::asio::co_spawn(executor,
					[self, socket = std::move(socket)]() mutable
						-> awaitable<void> {
						co_await self->session(std::move(socket));
					}, boost::asio::detached);
			} catch (const std::exception &error) {
				if (running)
					report(errors, "tcp", error.what());
			}
		}
	}

	boost::asio::any_io_executor executor;
	boost::asio::ip::tcp::acceptor acceptor;
	RequestHandler &handler;
	TcpServerConfig config;
	ErrorHandler errors;
	std::atomic<std::uint32_t> clients{0};
	std::atomic<bool> running{false};
	std::mutex sockets_mutex;
	std::set<std::shared_ptr<boost::asio::ip::tcp::socket>,
		std::owner_less<std::shared_ptr<boost::asio::ip::tcp::socket>>> sockets;
};

ModbusTcpServer::ModbusTcpServer(boost::asio::any_io_executor executor,
	RequestHandler &handler, TcpServerConfig config, ErrorHandler errors)
	: impl_(std::make_shared<Impl>(std::move(executor), handler,
		std::move(config), std::move(errors)))
{
}

ModbusTcpServer::~ModbusTcpServer() { stop(); }

void ModbusTcpServer::start()
{
	if (impl_->running.exchange(true))
		return;
	try {
		if (impl_->config.unit_id == 0 || impl_->config.unit_id > 247)
			throw std::invalid_argument("Modbus TCP unit id must be 1..247");
		if (impl_->config.maximum_clients == 0)
			throw std::invalid_argument("Modbus TCP maximum clients must be nonzero");
		const auto address = boost::asio::ip::make_address(
			impl_->config.bind_address);
		const boost::asio::ip::tcp::endpoint endpoint(address,
			impl_->config.port);
		impl_->acceptor.open(endpoint.protocol());
		impl_->acceptor.set_option(
			boost::asio::socket_base::reuse_address(true));
		impl_->acceptor.bind(endpoint);
		impl_->acceptor.listen();
		auto impl = impl_;
		boost::asio::co_spawn(impl_->executor,
			[impl]() -> awaitable<void> {
				co_await impl->accept_loop();
			}, boost::asio::detached);
	} catch (...) {
		impl_->running = false;
		throw;
	}
}

void ModbusTcpServer::stop() noexcept
{
	if (!impl_ || !impl_->running.exchange(false))
		return;
	boost::system::error_code ignored;
	impl_->acceptor.cancel(ignored);
	impl_->acceptor.close(ignored);
	std::lock_guard lock(impl_->sockets_mutex);
	for (const auto &socket : impl_->sockets) {
		socket->cancel(ignored);
		socket->close(ignored);
	}
}

std::uint16_t ModbusTcpServer::local_port() const
{
	return impl_->acceptor.local_endpoint().port();
}

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
	return byte(frame[frame.size() - 2]) == (expected & 0xffu) &&
	       byte(frame[frame.size() - 1]) == (expected >> 8u);
}

std::optional<std::size_t> RtuFrameAssembler::frame_size(
	std::span<const std::byte> bytes)
{
	if (bytes.size() < 2)
		return std::nullopt;
	const auto function = byte(bytes[1]);
	switch (static_cast<FunctionCode>(function)) {
	case FunctionCode::read_holding_registers:
	case FunctionCode::read_input_registers:
	case FunctionCode::write_single_register:
		return 8;
	case FunctionCode::write_multiple_registers:
		if (bytes.size() < 7)
			return std::nullopt;
		return 9 + byte(bytes[6]);
	default:
		/* Standard fixed-size request shape lets unsupported functions receive
		 * an Illegal Function response instead of poisoning the next frame. */
		return 8;
	}
}

std::vector<std::vector<std::byte>> RtuFrameAssembler::push(
	std::span<const std::byte> bytes)
{
	buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
	if (buffer_.size() > maximum_rtu_frame_size * 2) {
		buffer_.clear();
		throw std::length_error("RTU receive buffer exceeded its bound");
	}
	std::vector<std::vector<std::byte>> frames;
	for (;;) {
		const auto size = frame_size(buffer_);
		if (!size || buffer_.size() < *size)
			break;
		if (*size > maximum_rtu_frame_size) {
			buffer_.clear();
			throw std::length_error("RTU frame exceeded its bound");
		}
		frames.emplace_back(buffer_.begin(), buffer_.begin() + *size);
		buffer_.erase(buffer_.begin(), buffer_.begin() + *size);
	}
	return frames;
}

struct ModbusRtuServer::Impl {
	struct Port : std::enable_shared_from_this<Port> {
		Port(boost::asio::any_io_executor executor, RequestHandler &handler,
		     RtuPortConfig config, ErrorHandler errors,
		     std::shared_ptr<std::atomic<bool>> running)
			: serial(executor), gap_timer(std::move(executor)), handler(handler),
			  config(std::move(config)), errors(std::move(errors)),
			  running(std::move(running)),
			  gap_duration(character_gap(this->config))
		{
		}

		/**
		 * Modbus specifies an RTU frame boundary as at least 3.5 character
		 * times of silence. Above 19,200 baud the specification permits the
		 * fixed 1.75 ms interval. The timer complements function-aware length
		 * parsing: complete supported requests are handled immediately, while
		 * a truncated request is discarded before the next frame can arrive.
		 */
		static std::chrono::microseconds character_gap(
			const RtuPortConfig &config)
		{
			if (config.baud_rate > 19'200)
				return std::chrono::microseconds(1'750);
			const std::uint32_t parity_bits =
				config.parity == SerialParity::none ? 0u : 1u;
			const std::uint32_t bits_per_character = 1u + config.data_bits +
				parity_bits + config.stop_bits;
			const std::uint64_t numerator =
				3'500'000ull * bits_per_character;
			return std::chrono::microseconds(
				(numerator + config.baud_rate - 1) / config.baud_rate);
		}

		void arm_gap_timer()
		{
			gap_timer.expires_after(gap_duration);
			auto self = shared_from_this();
			gap_timer.async_wait([self](const boost::system::error_code &error) {
				if (error == boost::asio::error::operation_aborted ||
				    !*self->running)
					return;
				std::size_t discarded = 0;
				{
					std::scoped_lock lock(self->assembler_mutex);
					discarded = self->assembler.pending_size();
					self->assembler.clear();
				}
				if (discarded != 0)
					report(self->errors, "rtu", self->config.device +
						": discarded " + std::to_string(discarded) +
						" bytes after an incomplete RTU frame timeout");
			});
		}

		void open()
		{
			serial.open(config.device);
			serial.set_option(boost::asio::serial_port_base::baud_rate(
				config.baud_rate));
			serial.set_option(boost::asio::serial_port_base::character_size(
				config.data_bits));
			using Parity = boost::asio::serial_port_base::parity;
			const auto parity = config.parity == SerialParity::none
				? Parity::none : config.parity == SerialParity::even
					? Parity::even : Parity::odd;
			serial.set_option(Parity(parity));
			using Stop = boost::asio::serial_port_base::stop_bits;
			serial.set_option(Stop(config.stop_bits == 2
				? Stop::two : Stop::one));
		}

		awaitable<void> run()
		{
			try {
				while (*running) {
					const auto count = co_await serial.async_read_some(
						boost::asio::buffer(input), use_awaitable);
					boost::system::error_code ignored;
					gap_timer.cancel(ignored);
					std::vector<std::vector<std::byte>> frames;
					{
						std::scoped_lock lock(assembler_mutex);
						frames = assembler.push(std::span(input).first(count));
					}
					arm_gap_timer();
					for (auto &frame : frames) {
						if (!valid_crc(frame)) {
							report(errors, "rtu",
								config.device + ": CRC mismatch");
							continue;
						}
						const auto unit = byte(frame[0]);
						auto response = handler.handle(
							Request{unit, std::span(frame).subspan(
								1, frame.size() - 3), true},
							config.unit_id);
						if (!response)
							continue;
						std::vector<std::byte> output;
						output.reserve(response->pdu.size() + 3);
						output.push_back(static_cast<std::byte>(response->unit_id));
						output.insert(output.end(), response->pdu.begin(),
							response->pdu.end());
						const auto checksum = crc16(output);
						output.push_back(static_cast<std::byte>(checksum & 0xffu));
						output.push_back(static_cast<std::byte>(checksum >> 8u));
						co_await boost::asio::async_write(serial,
							boost::asio::buffer(output), use_awaitable);
					}
				}
			} catch (const std::exception &error) {
				if (*running)
					report(errors, "rtu", config.device + ": " + error.what());
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
		std::chrono::microseconds gap_duration;
		std::array<std::byte, 256> input{};
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

ModbusRtuServer::~ModbusRtuServer() { stop(); }

void ModbusRtuServer::start()
{
	if (impl_->running->exchange(true))
		return;
	std::size_t opened = 0;
	for (const auto &config : impl_->configs) {
		try {
			if (config.unit_id == 0 || config.unit_id > 247)
				throw std::invalid_argument("Modbus RTU unit id must be 1..247");
			if (config.device.empty())
				throw std::invalid_argument("Modbus RTU device path is empty");
			auto port = std::make_shared<Impl::Port>(impl_->executor,
				impl_->handler, config, impl_->errors, impl_->running);
			port->open();
			impl_->ports.push_back(port);
			boost::asio::co_spawn(impl_->executor,
				[port]() -> awaitable<void> {
					co_await port->run();
				}, boost::asio::detached);
			++opened;
		} catch (const std::exception &error) {
			report(impl_->errors, "rtu", config.device + ": " + error.what());
		}
	}
	(void)opened; // Each failed port was reported; other transports stay alive.
}

void ModbusRtuServer::stop() noexcept
{
	if (!impl_ || !impl_->running->exchange(false))
		return;
	for (const auto &port : impl_->ports) {
		boost::system::error_code ignored;
		port->gap_timer.cancel(ignored);
		port->serial.cancel(ignored);
		port->serial.close(ignored);
	}
	impl_->ports.clear();
}

} // namespace mnc::modbus
