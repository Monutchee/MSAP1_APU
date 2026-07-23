#include "msap1/acquisition_ipc.hpp"
#include "msap1/meter_config.hpp"
#include "msap1/meter_record.hpp"
#include "msap1/protocol.hpp"
#include "msap1/rpmsg_endpoint.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

volatile std::sig_atomic_t stop_requested = 0;

void handle_signal(int)
{
	stop_requested = 1;
}

[[noreturn]] void throw_errno(const std::string &operation)
{
	throw std::runtime_error(operation + ": " + std::strerror(errno));
}

struct Options {
	std::string service = "mncos-r5c0-ctrl";
	std::string rpmsg_device;
	std::string meter_device = "/dev/msap1-meter";
	std::string configuration = msap1::default_meter_config_path;
	std::string socket_path = msap1::acquisition_socket_path;
};

void usage(const char *program)
{
	std::cerr
		<< "Usage: " << program << " [options]\n"
		<< "  --service NAME       RPMsg service (default: mncos-r5c0-ctrl)\n"
		<< "  --rpmsg-device PATH  Use an existing /dev/rpmsgN endpoint\n"
		<< "  --meter-device PATH  Meter DMA device (default: /dev/msap1-meter)\n"
		<< "  --config PATH        Meter conversion JSON\n"
		<< "  --socket PATH        Control socket path\n";
}

Options parse_options(int argc, char **argv)
{
	Options options;
	for (int index = 1; index < argc; ++index) {
		const std::string option = argv[index];
		if (option == "--help" || option == "-h") {
			usage(argv[0]);
			std::exit(0);
		}
		if (index + 1 >= argc)
			throw std::invalid_argument(option + " requires a value");
		const std::string value = argv[++index];
		if (option == "--service")
			options.service = value;
		else if (option == "--rpmsg-device")
			options.rpmsg_device = value;
		else if (option == "--meter-device")
			options.meter_device = value;
		else if (option == "--config")
			options.configuration = value;
		else if (option == "--socket")
			options.socket_path = value;
		else
			throw std::invalid_argument("unknown option '" + option + "'");
	}
	return options;
}

class MeterDevice {
public:
	explicit MeterDevice(std::string path) : path_(std::move(path)) {}
	~MeterDevice() { stop(); }

	void start()
	{
		if (fd_ >= 0)
			return;
		fd_ = ::open(path_.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd_ < 0)
			throw_errno("open " + path_);
	}

	void stop() noexcept
	{
		if (fd_ >= 0)
			::close(fd_);
		fd_ = -1;
	}

	int fd() const { return fd_; }
	const std::string &path() const { return path_; }

private:
	std::string path_;
	int fd_ = -1;
};

class AcquisitionDaemon {
public:
	explicit AcquisitionDaemon(const Options &options)
		: options_(options),
		  configuration_(msap1::load_meter_configuration(
			  options.configuration)),
		  meter_(options.meter_device),
		  endpoint_(options.service, options.rpmsg_device)
	{
		create_socket();
	}

	~AcquisitionDaemon()
	{
		stop();
		if (listen_fd_ >= 0)
			::close(listen_fd_);
		if (!options_.socket_path.empty())
			::unlink(options_.socket_path.c_str());
	}

	void run()
	{
		start();
		std::cerr << "meter acquisition started: " << meter_.path()
			  << ", configuration generation "
			  << configuration_.wire.generation << '\n';
		auto next_health = Clock::now() + 1s;

		while (!stop_requested) {
			pollfd descriptors[2] = {
				{meter_.fd(), POLLIN, 0},
				{listen_fd_, POLLIN, 0},
			};
			const int result = ::poll(descriptors, 2, 250);
			if (result < 0) {
				if (errno == EINTR)
					continue;
				throw_errno("poll acquisition devices");
			}
			if ((descriptors[0].revents & POLLIN) != 0)
				read_meter_records();
			if ((descriptors[0].revents &
			     (POLLERR | POLLHUP | POLLNVAL)) != 0) {
				++dma_read_errors_;
				throw std::runtime_error("meter DMA device disconnected");
			}
			if ((descriptors[1].revents & POLLIN) != 0)
				handle_client();

			if (running_ && Clock::now() >= next_health) {
				try {
					cached_health_ = query_rpu_health();
				} catch (const std::exception &error) {
					std::cerr << "RPU health query failed: "
						  << error.what() << '\n';
				}
				next_health = Clock::now() + 1s;
			}
		}
	}

private:
	void create_socket()
	{
		const auto parent =
			std::filesystem::path(options_.socket_path).parent_path();
		if (!parent.empty())
			std::filesystem::create_directories(parent);
		listen_fd_ = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
		if (listen_fd_ < 0)
			throw_errno("create acquisition control socket");
		::unlink(options_.socket_path.c_str());
		sockaddr_un address{};
		address.sun_family = AF_UNIX;
		if (options_.socket_path.size() >= sizeof(address.sun_path))
			throw std::invalid_argument("acquisition socket path is too long");
		std::memcpy(address.sun_path, options_.socket_path.c_str(),
			    options_.socket_path.size() + 1);
		if (::bind(listen_fd_, reinterpret_cast<sockaddr *>(&address),
			   sizeof(address)) < 0)
			throw_errno("bind " + options_.socket_path);
		(void)::chmod(options_.socket_path.c_str(), 0660);
		if (::listen(listen_fd_, 8) < 0)
			throw_errno("listen on acquisition socket");
	}

	msap1::Message transact(std::uint8_t type, const void *payload = nullptr,
				std::size_t payload_size = 0,
				std::chrono::milliseconds timeout = 1000ms)
	{
		const auto sequence = ++rpmsg_sequence_;
		endpoint_.send(msap1::encode_request(
			type, sequence, payload, payload_size));
		const auto deadline = Clock::now() + timeout;
		while (Clock::now() < deadline) {
			const auto remaining =
				std::chrono::duration_cast<std::chrono::milliseconds>(
					deadline - Clock::now());
			const auto frame = endpoint_.receive(remaining);
			if (frame.empty())
				continue;
			auto message = msap1::decode_message(frame.data(), frame.size());
			if (message.header.sequence != sequence)
				continue;
			if (message.header.type == MSAP1_RPU_MSG_ERROR ||
			    message.header.status != MSAP1_RPU_STATUS_OK)
				throw std::runtime_error(
					"RPU rejected request (status " +
					std::to_string(message.header.status) + ")");
			return message;
		}
		throw std::runtime_error("timed out waiting for RPU response");
	}

	msap1_adc_health_payload query_rpu_health()
	{
		return msap1::decode_adc_health(
			transact(MSAP1_RPU_MSG_ADC_HEALTH_GET));
	}

	void configure_meter()
	{
		const auto response = transact(MSAP1_RPU_MSG_METER_CONFIG_SET,
			&configuration_.wire, sizeof(configuration_.wire));
		const auto acknowledgement =
			msap1::decode_meter_config_ack(response);
		if (acknowledgement.generation != configuration_.wire.generation ||
		    acknowledgement.conversion_active_generation !=
			configuration_.wire.generation ||
		    acknowledgement.processing_active_generation !=
			configuration_.wire.generation ||
		    (acknowledgement.conversion_status & 1u) == 0u ||
		    (acknowledgement.processing_status & 1u) == 0u)
			throw std::runtime_error(
				"RPU meter configuration readback does not match");
	}

	void start()
	{
		if (running_)
			return;
		meter_.start();
		try {
			// PGA and coefficient changes are a coordinated ADC/PL
			// transaction and may only occur with capture stopped. STOP is
			// idempotent, so this also recovers cleanly after a daemon crash.
			const auto stop_response =
				transact(MSAP1_RPU_MSG_ADC_CAPTURE_STOP);
			if (stop_response.header.type != MSAP1_RPU_MSG_ACK ||
			    !stop_response.payload.empty())
				throw std::runtime_error(
					"unexpected RPU capture-stop response");
			configure_meter();
			const auto response =
				transact(MSAP1_RPU_MSG_ADC_CAPTURE_START);
			if (response.header.type != MSAP1_RPU_MSG_ACK ||
			    !response.payload.empty())
				throw std::runtime_error(
					"unexpected RPU capture-start response");
			cached_health_ = query_rpu_health();
		} catch (...) {
			meter_.stop();
			throw;
		}
		running_ = true;
	}

	void stop() noexcept
	{
		if (!running_)
			return;
		try {
			(void)transact(MSAP1_RPU_MSG_ADC_CAPTURE_STOP, nullptr, 0,
				       500ms);
		} catch (const std::exception &error) {
			std::cerr << "RPU capture stop failed: " << error.what() << '\n';
		}
		meter_.stop();
		running_ = false;
	}

	void accept_record(const msap1::MeterRecord &record)
	{
		if (!record.header_valid() ||
		    record.configuration_generation() !=
			configuration_.wire.generation ||
		    record.sample_rate_hz() != configuration_.wire.sample_rate_hz ||
		    record.window_samples() !=
			configuration_.wire.rms_window_samples) {
			++invalid_records_;
			return;
		}

		if (latest_record_) {
			const auto expected = latest_record_->sequence() + 1u;
			const auto received = record.sequence();
			if (received != expected)
				sequence_gaps_ += static_cast<std::uint32_t>(
					received - expected);
		}
		latest_record_ = record;
		++meter_records_;
	}

	void read_meter_records()
	{
		std::array<msap1::MeterRecord, 16> records{};
		const auto size = ::read(meter_.fd(), records.data(), sizeof(records));
		if (size < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
				return;
			++dma_read_errors_;
			return;
		}
		if (size == 0)
			return;
		dma_bytes_ += static_cast<std::uint64_t>(size);
		if (size % static_cast<ssize_t>(sizeof(msap1::MeterRecord)) != 0) {
			++invalid_records_;
			return;
		}
		const auto count = static_cast<std::size_t>(size) /
			sizeof(msap1::MeterRecord);
		for (std::size_t index = 0; index < count; ++index)
			accept_record(records[index]);
	}

	msap1::AcquisitionResponse make_response(
		const msap1::AcquisitionRequest &request) const
	{
		msap1::AcquisitionResponse response{};
		response.sequence = request.sequence;
		response.running = running_ ? 1u : 0u;
		response.has_meter_record = latest_record_.has_value() ? 1u : 0u;
		response.sample_rate_hz = configuration_.wire.sample_rate_hz;
		response.configuration_generation = configuration_.wire.generation;
		response.meter_records = meter_records_;
		response.dma_bytes = dma_bytes_;
		response.dma_read_errors = dma_read_errors_;
		response.invalid_records = invalid_records_;
		response.sequence_gaps = sequence_gaps_;
		response.rpu_health = cached_health_;
		if (latest_record_)
			response.latest_record = *latest_record_;
		return response;
	}

	void handle_client()
	{
		const int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
		if (fd < 0)
			return;
		msap1::AcquisitionRequest request{};
		const auto size = ::recv(fd, &request, sizeof(request), 0);
		auto response = make_response(request);
		if (size != static_cast<ssize_t>(sizeof(request)) ||
		    request.magic != msap1::acquisition_ipc_magic ||
		    request.version != msap1::acquisition_ipc_version) {
			response.status = msap1::AcquisitionStatus::bad_request;
		} else {
			try {
				switch (request.command) {
				case msap1::AcquisitionCommand::info:
					break;
				case msap1::AcquisitionCommand::health:
					cached_health_ = query_rpu_health();
					break;
				case msap1::AcquisitionCommand::start:
					start();
					break;
				case msap1::AcquisitionCommand::stop:
					stop();
					break;
				default:
					response.status =
						msap1::AcquisitionStatus::bad_request;
					break;
				}
			} catch (const std::exception &error) {
				std::cerr << "acquisition command failed: "
					  << error.what() << '\n';
				if (request.command == msap1::AcquisitionCommand::health)
					response.status =
						msap1::AcquisitionStatus::rpu_error;
				else if (request.command ==
					 msap1::AcquisitionCommand::start)
					response.status =
						msap1::AcquisitionStatus::configuration_error;
				else
					response.status =
						msap1::AcquisitionStatus::dma_error;
			}
		}

		const auto current = make_response(request);
		const auto saved_status = response.status;
		response = current;
		response.status = saved_status;
		(void)::send(fd, &response, sizeof(response), MSG_NOSIGNAL);
		::close(fd);
	}

	Options options_;
	msap1::PreparedMeterConfiguration configuration_;
	MeterDevice meter_;
	msap1::RpmsgEndpoint endpoint_;
	int listen_fd_ = -1;
	std::uint32_t rpmsg_sequence_ = 0x90000000u;
	bool running_ = false;
	std::uint64_t meter_records_ = 0;
	std::uint64_t dma_bytes_ = 0;
	std::uint64_t dma_read_errors_ = 0;
	std::uint64_t invalid_records_ = 0;
	std::uint64_t sequence_gaps_ = 0;
	std::optional<msap1::MeterRecord> latest_record_;
	msap1_adc_health_payload cached_health_{};
};

} // namespace

int main(int argc, char **argv)
{
	std::signal(SIGINT, handle_signal);
	std::signal(SIGTERM, handle_signal);
	try {
		AcquisitionDaemon daemon(parse_options(argc, argv));
		daemon.run();
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "msap1-fpga-acquisition: " << error.what() << '\n';
		return 1;
	}
}
