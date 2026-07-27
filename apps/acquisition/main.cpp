#include "msap1/acquisition_ipc.hpp"
#include "msap1/meter_config.hpp"
#include "msap1/meter_record.hpp"
#include "msap1/protocol.hpp"
#include "msap1/rpmsg_endpoint.hpp"
#include "mnc/logging/logging.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <initializer_list>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <utility>

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

const mnc::logging::Logger lifecycle_log("fpga-acquisition", "lifecycle");
const mnc::logging::Logger dma_log("fpga-acquisition", "dma");
const mnc::logging::Logger rpmsg_log("fpga-acquisition", "rpmsg");
const mnc::logging::Logger config_log("fpga-acquisition", "adc-config");
const mnc::logging::Logger health_log("fpga-acquisition", "health");

void log_message(
	const mnc::logging::Logger &logger, mnc::logging::Priority priority,
	std::string message, std::string_view event,
	std::initializer_list<mnc::logging::Field> fields = {},
	const std::source_location &source = std::source_location::current())
{
	(void)logger.write(priority, message, event,
			   std::span<const mnc::logging::Field>(
				   fields.begin(), fields.size()),
			   source);
}

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
	std::string active_configuration =
		"/etc/monutchee/msap1/adc_config/active.json";
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
		<< "  --active-config PATH Persisted complete runtime profile\n"
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
		else if (option == "--active-config")
			options.active_configuration = value;
		else if (option == "--socket")
			options.socket_path = value;
		else
			throw std::invalid_argument("unknown option '" + option + "'");
	}
	return options;
}

msap1::PreparedMeterConfiguration load_runtime_configuration(
	const Options &options)
{
	if (!options.active_configuration.empty() &&
	    std::filesystem::exists(options.active_configuration)) {
		try {
			return msap1::load_meter_configuration(
				options.active_configuration);
		} catch (const std::exception &error) {
			log_message(config_log, mnc::logging::Priority::warning,
				"ignoring invalid active meter profile " +
					options.active_configuration + ": " +
					error.what(),
				"active_profile_invalid",
				{{"MNC_CONFIG_PATH",
				  options.active_configuration}});
		}
	}
	return msap1::load_meter_configuration(options.configuration);
}

std::string frequency_mode_name(std::uint32_t mode)
{
	switch (mode) {
	case MSAP1_FREQUENCY_MODE_SINGLE_CYCLE: return "single_cycle";
	case MSAP1_FREQUENCY_MODE_ROLLING_CYCLES: return "rolling_cycles";
	case MSAP1_FREQUENCY_MODE_ROLLING_TIME: return "rolling_time";
	default: throw std::runtime_error("invalid frequency IPC mode");
	}
}

msap1::FrequencyIpcConfiguration frequency_ipc(
	const msap1::FrequencyConfig &frequency)
{
	std::uint32_t mode = MSAP1_FREQUENCY_MODE_ROLLING_CYCLES;
	if (frequency.mode == "single_cycle")
		mode = MSAP1_FREQUENCY_MODE_SINGLE_CYCLE;
	else if (frequency.mode == "rolling_time")
		mode = MSAP1_FREQUENCY_MODE_ROLLING_TIME;
	return {
		frequency.enabled ? 1u : 0u,
		frequency.reference_channel,
		mode,
		frequency.averaging_cycles,
		frequency.averaging_window_ms,
		static_cast<std::uint32_t>(
			std::llround(frequency.minimum_hz * 1000.0)),
		static_cast<std::uint32_t>(
			std::llround(frequency.maximum_hz * 1000.0)),
		static_cast<std::uint32_t>(
			std::llround(frequency.hysteresis_volts * 1000000.0)),
	};
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
		log_message(dma_log, mnc::logging::Priority::info,
			"meter DMA device opened: " + path_, "dma_opened",
			{{"MNC_DEVICE", path_}});
	}

	void stop() noexcept
	{
		if (fd_ >= 0) {
			::close(fd_);
			log_message(dma_log, mnc::logging::Priority::info,
				"meter DMA device closed: " + path_, "dma_closed",
				{{"MNC_DEVICE", path_}});
		}
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
		  configuration_(load_runtime_configuration(options)),
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
		log_message(lifecycle_log, mnc::logging::Priority::notice,
			"meter acquisition started: " + meter_.path() +
				", configuration generation " +
				std::to_string(configuration_.wire.generation),
			"service_started",
			{{"MNC_CONFIGURATION_GENERATION",
			  std::to_string(configuration_.wire.generation)},
			 {"MNC_DEVICE", meter_.path()}});
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
				log_message(dma_log, mnc::logging::Priority::error,
					"meter DMA device disconnected",
					"dma_disconnected");
				throw std::runtime_error("meter DMA device disconnected");
			}
			if ((descriptors[1].revents & POLLIN) != 0)
				handle_client();

			if (running_ && Clock::now() >= next_health) {
				try {
					cached_health_ = query_rpu_health();
				} catch (const std::exception &error) {
					log_message(rpmsg_log,
						mnc::logging::Priority::warning,
						"RPU health query failed: " +
							std::string(error.what()),
						"health_query_failed");
				}
				next_health = Clock::now() + 1s;
			}
		}
		log_message(lifecycle_log, mnc::logging::Priority::notice,
			"meter acquisition service is stopping",
			"service_stopping");
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
			    message.header.status != MSAP1_RPU_STATUS_OK) {
				log_message(rpmsg_log,
					mnc::logging::Priority::error,
					"RPU rejected request with status " +
						std::to_string(
							message.header.status),
					"request_rejected",
					{{"MNC_REQUEST_ID",
					  std::to_string(sequence)},
					 {"MNC_RPU_STATUS",
					  std::to_string(
						  message.header.status)}});
				throw std::runtime_error(
					"RPU rejected request (status " +
					std::to_string(message.header.status) + ")");
			}
			return message;
		}
		log_message(rpmsg_log, mnc::logging::Priority::error,
			"timed out waiting for RPU response", "request_timeout",
			{{"MNC_REQUEST_ID", std::to_string(sequence)}});
		throw std::runtime_error("timed out waiting for RPU response");
	}

	msap1_adc_health_payload query_rpu_health()
	{
		auto health = msap1::decode_adc_health(
			transact(MSAP1_RPU_MSG_ADC_HEALTH_GET));
		observe_rpu_health(health);
		return health;
	}

	void observe_rpu_health(const msap1_adc_health_payload &health)
	{
		if (last_health_flags_ &&
		    *last_health_flags_ == health.health_flags &&
		    last_spi_error_ == health.spi_error)
			return;
		constexpr std::uint32_t expected =
			MSAP1_ADC_HEALTH_SPI_RESPONSIVE |
			MSAP1_ADC_HEALTH_INITIALIZED |
			MSAP1_ADC_HEALTH_INIT_COMPLETE |
			MSAP1_ADC_HEALTH_CONFIG_MATCH |
			MSAP1_ADC_HEALTH_CAPTURE_ACTIVE |
			MSAP1_ADC_HEALTH_NO_OVERFLOW |
			MSAP1_ADC_HEALTH_HEADERS_VALID |
			MSAP1_ADC_HEALTH_RATE_MATCH;
		const bool healthy = (health.health_flags & expected) == expected &&
			health.spi_error == MSAP1_ADC_SPI_HEALTH_OK;
		log_message(health_log,
			healthy ? mnc::logging::Priority::notice
				: mnc::logging::Priority::warning,
			healthy ? "RPU ADC health became healthy"
				: "RPU ADC health became degraded",
			healthy ? "rpu_health_healthy" : "rpu_health_degraded",
			{{"MNC_ADC_HEALTH_FLAGS",
			  std::to_string(health.health_flags)},
			 {"MNC_SPI_ERROR", std::to_string(health.spi_error)},
			 {"MNC_CONFIGURATION_GENERATION",
			  std::to_string(health.meter_generation)}});
		last_health_flags_ = health.health_flags;
		last_spi_error_ = health.spi_error;
	}

	void configure_meter()
	{
		log_message(config_log, mnc::logging::Priority::info,
			"applying coordinated ADC and PL meter configuration",
			"configuration_apply_started",
			{{"MNC_CONFIGURATION_GENERATION",
			  std::to_string(configuration_.wire.generation)},
			 {"MNC_SAMPLE_RATE_HZ",
			  std::to_string(configuration_.wire.sample_rate_hz)}});
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
		log_message(config_log, mnc::logging::Priority::notice,
			"coordinated ADC and PL meter configuration applied",
			"configuration_applied",
			{{"MNC_CONFIGURATION_GENERATION",
			  std::to_string(configuration_.wire.generation)},
			 {"MNC_SAMPLE_RATE_HZ",
			  std::to_string(configuration_.wire.sample_rate_hz)}});
	}

	void start(bool apply_configuration = true)
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
			if (apply_configuration)
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
		log_message(lifecycle_log, mnc::logging::Priority::notice,
			"ADC capture and meter DMA started", "capture_started",
			{{"MNC_CONFIGURATION_GENERATION",
			  std::to_string(configuration_.wire.generation)}});
	}

	void stop() noexcept
	{
		if (!running_)
			return;
		try {
			(void)transact(MSAP1_RPU_MSG_ADC_CAPTURE_STOP, nullptr, 0,
				       500ms);
		} catch (const std::exception &error) {
			log_message(rpmsg_log, mnc::logging::Priority::warning,
				"RPU capture stop failed: " +
					std::string(error.what()),
				"capture_stop_failed");
		}
		meter_.stop();
		running_ = false;
		log_message(lifecycle_log, mnc::logging::Priority::notice,
			"ADC capture and meter DMA stopped", "capture_stopped");
	}

	void apply_frequency_configuration(
		const msap1::FrequencyIpcConfiguration &request)
	{
		auto source = configuration_.source;
		source.frequency.enabled = request.enabled != 0u;
		source.frequency.reference_channel = request.reference_channel;
		source.frequency.mode = frequency_mode_name(request.mode);
		source.frequency.averaging_cycles = request.averaging_cycles;
		source.frequency.averaging_window_ms =
			request.averaging_window_ms;
		source.frequency.minimum_hz =
			static_cast<double>(request.minimum_millihz) / 1000.0;
		source.frequency.maximum_hz =
			static_cast<double>(request.maximum_millihz) / 1000.0;
		source.frequency.hysteresis_volts =
			static_cast<double>(request.hysteresis_microvolts) /
			1000000.0;

		auto staged = msap1::prepare_meter_configuration(
			std::move(source), configuration_.wire.sample_rate_hz);
		const auto previous = configuration_;
		const bool restart = running_;
		if (restart)
			stop();
		configuration_ = std::move(staged);
		// The previous snapshot belongs to a different configuration
		// generation. Publish no record until the restarted pipeline produces
		// a coherent MTR1 result for the new generation.
		latest_record_.reset();
		try {
			if (restart)
				start();
			msap1::save_meter_configuration(
				configuration_.source,
				options_.active_configuration);
			log_message(config_log, mnc::logging::Priority::notice,
				"frequency configuration applied and persisted",
				"frequency_configuration_applied",
				{{"MNC_CONFIGURATION_GENERATION",
				  std::to_string(
					  configuration_.wire.generation)}});
		} catch (...) {
			if (running_)
				stop();
			configuration_ = previous;
			if (restart) {
				try {
					start();
				} catch (const std::exception &rollback_error) {
					log_message(config_log,
						mnc::logging::Priority::critical,
						"frequency configuration rollback failed: " +
							std::string(
								rollback_error.what()),
						"frequency_rollback_failed");
				}
			}
			throw;
		}
	}

	void apply_sample_rate(std::uint32_t sample_rate_hz)
	{
		if (!msap1::supported_adc_sample_rate(sample_rate_hz))
			throw std::invalid_argument("unsupported ADC sample rate");

		auto staged = msap1::prepare_meter_configuration(
			configuration_.source, sample_rate_hz);
		const auto previous = configuration_;
		const bool restart = running_;
		if (restart)
			stop();
		configuration_ = std::move(staged);
		latest_record_.reset();

		try {
			if (restart) {
				// start() arms DMA before committing the coordinated ADC/PL
				// configuration and requesting capture.
				start();
			} else {
				// A stopped pipeline still applies the operating point now
				// so `mnc adc rate` can diagnose DRDY without first
				// starting DMA and capture.
				configure_meter();
				cached_health_ = query_rpu_health();
			}
		} catch (...) {
			if (running_)
				stop();
			configuration_ = previous;
			latest_record_.reset();
			try {
				if (restart) {
					start();
				} else {
					configure_meter();
					cached_health_ = query_rpu_health();
				}
			} catch (const std::exception &rollback_error) {
				log_message(config_log,
					mnc::logging::Priority::critical,
					"sample-rate rollback failed: " +
						std::string(
							rollback_error.what()),
					"sample_rate_rollback_failed");
			}
			throw;
		}
		log_message(config_log, mnc::logging::Priority::notice,
			"temporary ADC sample rate applied: " +
				std::to_string(sample_rate_hz) + " frame/s",
			"sample_rate_applied",
			{{"MNC_SAMPLE_RATE_HZ",
			  std::to_string(sample_rate_hz)},
			 {"MNC_CONFIGURATION_GENERATION",
			  std::to_string(configuration_.wire.generation)}});
	}

	void run_adc_diagnostic(std::uint32_t flow)
	{
		if (flow != 1u)
			throw std::invalid_argument("unsupported ADC diagnostic flow");

		const bool restart = running_;
		if (restart)
			stop();

		try {
			const msap1_adc_diagnostic_request request{flow};
			last_adc_diagnostic_ = msap1::decode_adc_diagnostic(
				transact(MSAP1_RPU_MSG_ADC_DIAGNOSTIC_RUN,
					 &request, sizeof(request), 15000ms));

			if (restart) {
				/*
				 * A successful flow restores the same ADC operating point
				 * itself. Resume DMA/capture without issuing a second SRC
				 * load, otherwise the final diagnostic snapshot would no
				 * longer describe the active state. If the flow failed,
				 * perform the normal full coordinated configuration as a
				 * recovery attempt.
				 */
				const bool diagnostic_succeeded =
					last_adc_diagnostic_.diagnostic_error ==
					MSAP1_ADC_DIAGNOSTIC_ERROR_NONE;
				start(!diagnostic_succeeded);
			}
			cached_health_ = query_rpu_health();
			log_message(config_log, mnc::logging::Priority::notice,
				"ADC diagnostic flow completed",
				"adc_diagnostic_completed",
				{{"MNC_DIAGNOSTIC_FLOW", std::to_string(flow)},
				 {"MNC_DIAGNOSTIC_ERROR",
				  std::to_string(last_adc_diagnostic_
							 .diagnostic_error)}});
		} catch (...) {
			if (restart && !running_) {
				try {
					start();
				} catch (const std::exception &rollback_error) {
					log_message(config_log,
						mnc::logging::Priority::critical,
						"ADC diagnostic recovery failed: " +
							std::string(
								rollback_error.what()),
						"adc_diagnostic_recovery_failed");
				}
			}
			throw;
		}
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
			log_message(dma_log, mnc::logging::Priority::error,
				"meter DMA read failed: " +
					std::string(std::strerror(errno)),
				"dma_read_failed");
			return;
		}
		if (size == 0)
			return;
		dma_bytes_ += static_cast<std::uint64_t>(size);
		if (size % static_cast<ssize_t>(sizeof(msap1::MeterRecord)) != 0) {
			++invalid_records_;
			log_message(dma_log, mnc::logging::Priority::warning,
				"meter DMA returned a partial record",
				"dma_partial_record",
				{{"MNC_DMA_BYTES", std::to_string(size)}});
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
		response.adc_diagnostic = last_adc_diagnostic_;
		if (latest_record_)
			response.latest_record = *latest_record_;
		response.frequency = frequency_ipc(configuration_.source.frequency);
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
				case msap1::AcquisitionCommand::
					frequency_configuration_get:
					break;
				case msap1::AcquisitionCommand::
					frequency_configuration_set:
					apply_frequency_configuration(request.frequency);
					break;
				case msap1::AcquisitionCommand::sample_rate_set:
					apply_sample_rate(request.sample_rate_hz);
					break;
				case msap1::AcquisitionCommand::adc_diagnostic_run:
					run_adc_diagnostic(request.diagnostic_flow);
					break;
				default:
					response.status =
						msap1::AcquisitionStatus::bad_request;
					break;
				}
			} catch (const std::exception &error) {
				log_message(lifecycle_log,
					mnc::logging::Priority::error,
					"acquisition command failed: " +
						std::string(error.what()),
					"command_failed",
					{{"MNC_REQUEST_ID",
					  std::to_string(request.sequence)},
					 {"MNC_COMMAND",
					  std::to_string(static_cast<std::uint32_t>(
						  request.command))}});
				if (request.command == msap1::AcquisitionCommand::health)
					response.status =
						msap1::AcquisitionStatus::rpu_error;
				else if (request.command ==
					 msap1::AcquisitionCommand::start ||
					 request.command ==
					 msap1::AcquisitionCommand::
						 frequency_configuration_set ||
					 request.command ==
					 msap1::AcquisitionCommand::sample_rate_set ||
					 request.command ==
					 msap1::AcquisitionCommand::
						 adc_diagnostic_run)
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
	msap1_adc_diagnostic_payload last_adc_diagnostic_{};
	std::optional<std::uint32_t> last_health_flags_;
	std::uint32_t last_spi_error_ = 0;
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
		log_message(lifecycle_log, mnc::logging::Priority::critical,
			"msap1-fpga-acquisition: " + std::string(error.what()),
			"service_failed");
		return 1;
	}
}
