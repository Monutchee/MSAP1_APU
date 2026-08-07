#include "msap1/acquisition/ipc/acquisition_ipc.hpp"
#include "msap1/acquisition/dma/meter_dma_reader.hpp"
#include "msap1/acquisition/rpu/rpu_controller.hpp"
#include "msap1/meter/meter_config.hpp"
#include "msap1/meter/meter_data.hpp"
#include "msap1/meter/meter_health.hpp"
#include "msap1/meter/meter_record.hpp"
#include "msap1/meter/meter_record_stream.hpp"
#include "msap1/acquisition/rpu/protocol.hpp"
#include "msap1/settings/settings.hpp"
#include "msap1/settings/settings_ipc.hpp"
#include "msap1/waveform/waveform_capture.hpp"
#include "mnc/logging/logging.hpp"
#include "mnc/service/service.hpp"

#include <boost/asio/io_context.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstring>
#include <deque>
#include <filesystem>
#include <iostream>
#include <initializer_list>
#include <limits>
#include <mutex>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

constexpr auto health_audit_interval = 30s;
constexpr auto health_confirmation_interval = 1s;
/*
 * The PL DCLK/DRDY monitor publishes one-second snapshots. Delay the first
 * post-start audit long enough for a complete capture-active window to replace
 * the zero-rate snapshot produced while capture was stopped.
 */
constexpr auto health_startup_settle_interval = 2s;
constexpr std::uint32_t health_failures_before_degraded = 2;

volatile std::sig_atomic_t acquisition_stop_requested = 0;

const mnc::logging::Logger lifecycle_log("fpga-acquisition", "lifecycle");
const mnc::logging::Logger dma_log("fpga-acquisition", "dma");
const mnc::logging::Logger rpmsg_log("fpga-acquisition", "rpmsg");
const mnc::logging::Logger config_log("fpga-acquisition", "adc-config");
const mnc::logging::Logger health_log("fpga-acquisition", "health");
const mnc::logging::Logger waveform_log("fpga-acquisition", "waveform");

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

std::string health_reason_codes(
	const std::vector<msap1::HealthReason> &reasons)
{
	std::string result;
	for (const auto &reason : reasons) {
		if (!result.empty())
			result += ',';
		result += reason.code;
	}
	return result;
}

std::string health_reason_messages(
	const std::vector<msap1::HealthReason> &reasons)
{
	std::string result;
	for (const auto &reason : reasons) {
		if (!result.empty())
			result += "; ";
		result += reason.message;
	}
	return result;
}

[[noreturn]] void throw_errno(const std::string &operation)
{
	throw std::runtime_error(operation + ": " + std::strerror(errno));
}

/** Owns the eventfd used to hand IPC work from Asio to the acquisition loop. */
class EventSignal final {
public:
	EventSignal() = default;
	~EventSignal()
	{
		if (fd_ >= 0)
			::close(fd_);
	}
	EventSignal(const EventSignal &) = delete;
	EventSignal &operator=(const EventSignal &) = delete;

	void open()
	{
		if (fd_ >= 0)
			return;
		fd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
		if (fd_ < 0)
			throw_errno("create acquisition IPC eventfd");
	}

	[[nodiscard]] int native_handle() const noexcept { return fd_; }

	void notify() noexcept
	{
		const std::uint64_t value = 1;
		while (::write(fd_, &value, sizeof(value)) < 0 && errno == EINTR) {
		}
	}

	void consume() noexcept
	{
		std::uint64_t value = 0;
		while (::read(fd_, &value, sizeof(value)) < 0 && errno == EINTR) {
		}
	}

private:
	int fd_ = -1;
};

struct Options {
	std::string service = "mncos-r5c0-ctrl";
	std::string rpmsg_device;
	std::string meter_device = "/dev/msap1-meter";
	std::string waveform_device = "/dev/msap1-waveform";
	std::string waveform_directory = "/data/mnc/waveform";
	std::string socket_path = msap1::acquisition_socket_path;
	std::string record_stream = "/data/mnc/meter/record-stream.sqlite3";
};

void usage(const char *program)
{
	std::cerr
		<< "Usage: " << program << " [options]\n"
		<< "  --service NAME       RPMsg service (default: mncos-r5c0-ctrl)\n"
		<< "  --rpmsg-device PATH  Use an existing /dev/rpmsgN endpoint\n"
		<< "  --meter-device PATH  Meter DMA device (default: /dev/msap1-meter)\n"
		<< "  --waveform-device PATH Waveform DMA device (default: /dev/msap1-waveform)\n"
		<< "  --waveform-directory PATH Completed waveform storage\n"
		<< "  --socket PATH        Control socket path\n"
		<< "  --record-stream PATH Durable meter record database\n";
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
		else if (option == "--waveform-device")
			options.waveform_device = value;
		else if (option == "--waveform-directory")
			options.waveform_directory = value;
		else if (option == "--socket")
			options.socket_path = value;
		else if (option == "--record-stream")
			options.record_stream = value;
		else
			throw std::invalid_argument("unknown option '" + option + "'");
	}
	return options;
}

msap1::settings::ProductSettings load_runtime_settings()
{
	msap1::settings::ipc::SettingsClient client;
	return client.active(5000);
}

std::array<msap1::WaveformChannelMetadata,
	   msap1::waveform_persisted_channels>
waveform_metadata(const msap1::PreparedMeterConfiguration &configuration)
{
	static constexpr std::array<const char *,
				    msap1::waveform_persisted_channels>
		names{"Ia", "Ib", "Ic", "In", "Vc", "Vb", "Va"};
	std::array<msap1::WaveformChannelMetadata,
		   msap1::waveform_persisted_channels>
		result{};
	for (std::size_t channel = 0; channel < result.size(); ++channel) {
		auto &metadata = result[channel];
		metadata.source_channel = static_cast<std::uint32_t>(channel);
		metadata.kind = channel < 4u
			? msap1::WaveformChannelKind::current
			: msap1::WaveformChannelKind::voltage;
		metadata.scale_micro_units_q16 =
			configuration.wire.scale_micro_units_q16[channel];
		metadata.flags =
			(configuration.wire.valid_mask & (1u << channel)) != 0u
			? 1u
			: 0u;
		std::copy_n(names[channel],
			    std::min(std::strlen(names[channel]),
				     metadata.name.size() - 1u),
			    metadata.name.begin());
		const char *unit = channel < 4u ? "A" : "V";
		std::copy_n(unit, 1u, metadata.unit.begin());
	}
	return result;
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

msap1::SimulatorIpcConfiguration simulator_ipc(
	const msap1::SimulatorConfig &simulator)
{
	msap1::SimulatorIpcConfiguration result{};
	result.frequency_millihz = static_cast<std::uint32_t>(
		std::llround(simulator.frequency_hz * 1000.0));
	for (const auto &channel : simulator.channels) {
		if (channel.channel >= result.channels.size())
			continue;
		result.channels[channel.channel].rms = channel.rms;
		result.channels[channel.channel].phase_degrees =
			channel.phase_degrees;
	}
	return result;
}

class CaptureCoordinator {
public:
	explicit CaptureCoordinator(const Options &options)
		: options_(options),
		  product_settings_(load_runtime_settings()),
		  configuration_(msap1::prepare_meter_configuration(
			  msap1::settings::to_meter_configuration(product_settings_),
			  product_settings_.metering.sample_rate_hz)),
		  meter_(options.meter_device),
		  waveform_(options.waveform_device, options.waveform_directory,
			    waveform_metadata(configuration_)),
		  rpu_(options.service, options.rpmsg_device),
		  record_stream_(options.record_stream)
	{
		configure_registry();
		create_ipc_server();
	}

	~CaptureCoordinator()
	{
		stop();
		if (ipc_server_)
			ipc_server_->stop();
		ipc_context_.stop();
		if (ipc_thread_.joinable())
			ipc_thread_.join();
	}

	void run()
	{
		start();
		log_message(lifecycle_log, mnc::logging::Priority::notice,
			"meter acquisition started: " + std::string(meter_.name()) +
				", configuration generation " +
				std::to_string(configuration_.wire.generation),
			"service_started",
			{{"MNC_CONFIGURATION_GENERATION",
			  std::to_string(configuration_.wire.generation)},
			 {"MNC_DEVICE", std::string(meter_.name())}});
		while (!acquisition_stop_requested) {
			pollfd descriptors[3] = {
				{meter_.native_handle(), POLLIN, 0},
				{waveform_.fd(), POLLIN, 0},
				{ipc_event_.native_handle(), POLLIN, 0},
			};
			const int result = ::poll(descriptors, 3, 250);
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
			if ((descriptors[1].revents & POLLIN) != 0) {
				try {
					waveform_.read_available();
				} catch (const std::exception &error) {
					log_message(waveform_log,
						mnc::logging::Priority::error,
						"waveform DMA read failed: " +
							std::string(error.what()),
						"waveform_read_failed");
				}
			}
			if ((descriptors[1].revents &
			     (POLLERR | POLLHUP | POLLNVAL)) != 0)
				throw std::runtime_error(
					"waveform DMA device disconnected");
			if ((descriptors[2].revents & POLLIN) != 0)
				drain_ipc_requests();

			if (running_ && Clock::now() >= next_health_audit_) {
				try {
					refresh_rpu_health();
				} catch (const std::exception &error) {
					log_message(rpmsg_log,
						mnc::logging::Priority::warning,
						"RPU health query failed: " +
							std::string(error.what()),
						"health_query_failed",
						{{"MNC_CONSECUTIVE_FAILURES",
						  std::to_string(
							  health_probe_failures_)}});
					next_health_audit_ =
						Clock::now() +
						health_confirmation_interval;
				}
			}
		}
		log_message(lifecycle_log, mnc::logging::Priority::notice,
			"meter acquisition service is stopping",
			"service_stopping");
	}

private:
	struct PendingIpcRequest {
		std::shared_ptr<mnc::ipc::FramedConnection> connection;
		mnc::ipc::Frame frame;
	};

	void create_ipc_server()
	{
		ipc_event_.open();
		ipc_server_ = std::make_unique<mnc::ipc::UnixStreamServer>(
			ipc_context_.get_executor(), options_.socket_path);
		ipc_server_->start(
			[this](auto connection, auto frame) {
				{
					std::scoped_lock lock(ipc_mutex_);
					ipc_requests_.push_back(
						{std::move(connection), std::move(frame)});
				}
				ipc_event_.notify();
			},
			[](const std::string &message) {
				log_message(lifecycle_log,
					mnc::logging::Priority::warning,
					"acquisition IPC connection failed: " + message,
					"ipc_connection_failed");
			});
		ipc_thread_ = std::thread([this] { ipc_context_.run(); });
	}

	msap1::Message transact(std::uint8_t type, const void *payload = nullptr,
				std::size_t payload_size = 0,
				std::chrono::milliseconds timeout = 1000ms)
	{
		return rpu_.transact(type, payload, payload_size, timeout);
	}

	msap1_adc_health_payload query_rpu_health_raw()
	{
		return rpu_.query_health();
	}

	void observe_spi_recovery(const msap1_adc_health_payload &health)
	{
		if (health.spi_retry_recovery_count ==
		    last_spi_retry_recovery_count_)
			return;
		log_message(
			health_log, mnc::logging::Priority::notice,
			"ADC SPI register read recovered after retry",
			"spi_retry_recovered",
			{{"MNC_SPI_RETRY_RECOVERIES",
			  std::to_string(health.spi_retry_recovery_count)},
			 {"MNC_SPI_PROTOCOL_ERRORS",
			  std::to_string(health.spi_protocol_error_count)},
			 {"MNC_SPI_REGISTER",
			  std::to_string(health.spi_last_failed_register)},
			 {"MNC_SPI_RECEIVED_HEADER",
			  std::to_string(health.spi_last_received_header)}});
		last_spi_retry_recovery_count_ =
			health.spi_retry_recovery_count;
	}

	void update_cached_operational_health(
		const msap1_adc_health_payload &health)
	{
		/*
		 * Capture and MeterCore status do not depend on the failed SPI
		 * snapshot. Keep these fast fields current while retaining the last
		 * verified register-derived flags and register bytes.
		 */
		constexpr std::uint32_t register_health_flags =
			MSAP1_ADC_HEALTH_SPI_RESPONSIVE |
			MSAP1_ADC_HEALTH_INIT_COMPLETE |
			MSAP1_ADC_HEALTH_CONFIG_MATCH;
		cached_health_.health_flags =
			(cached_health_.health_flags & register_health_flags) |
			(health.health_flags & ~register_health_flags);
		cached_health_.meter_health_flags = health.meter_health_flags;
		cached_health_.meter_generation = health.meter_generation;
		cached_health_.conversion_status = health.conversion_status;
		cached_health_.processing_status = health.processing_status;
		cached_health_.sample_rate_hz = health.sample_rate_hz;
		cached_health_.capture_flags = health.capture_flags;
		cached_health_.frame_count = health.frame_count;
		cached_health_.overflow_count = health.overflow_count;
		cached_health_.header_error_count = health.header_error_count;
		cached_health_.alert_count = health.alert_count;
		cached_health_.packet_count = health.packet_count;
		cached_health_.dclk_frequency_hz = health.dclk_frequency_hz;
		cached_health_.drdy_frequency_hz = health.drdy_frequency_hz;
		cached_health_.spi_protocol_error_count =
			health.spi_protocol_error_count;
		cached_health_.spi_retry_recovery_count =
			health.spi_retry_recovery_count;
		cached_health_.spi_last_failed_register =
			health.spi_last_failed_register;
		cached_health_.spi_last_received_header =
			health.spi_last_received_header;
	}

	void refresh_rpu_health()
	{
		health_stabilizing_ = false;
		msap1_adc_health_payload health{};
		try {
			health = query_rpu_health_raw();
		} catch (...) {
			++health_probe_failures_;
			next_health_audit_ =
				Clock::now() + health_confirmation_interval;
			throw;
		}
		if (health.adc_source == MSAP1_ADC_SOURCE_SIMULATOR) {
			health_probe_failures_ = 0;
			cached_health_ = health;
			has_cached_health_ = true;
			last_rpu_health_time_ = Clock::now();
			next_health_audit_ = Clock::now() + health_audit_interval;
			observe_rpu_health(health);
			return;
		}
		const bool spi_snapshot_valid =
			(health.health_flags &
			 MSAP1_ADC_HEALTH_SPI_RESPONSIVE) != 0u &&
			health.spi_error == MSAP1_ADC_SPI_HEALTH_OK;

		if (spi_snapshot_valid) {
			health_probe_failures_ = 0;
			cached_health_ = health;
			has_cached_health_ = true;
			last_rpu_health_time_ = Clock::now();
			next_health_audit_ =
				Clock::now() + health_audit_interval;
			observe_spi_recovery(health);
			observe_rpu_health(health);
			return;
		}

		++health_probe_failures_;
		next_health_audit_ =
			Clock::now() + health_confirmation_interval;
		if (health_probe_failures_ < health_failures_before_degraded) {
			log_message(
				health_log, mnc::logging::Priority::notice,
				"transient ADC SPI health audit failure; confirmation scheduled",
				"rpu_health_confirmation_pending",
				{{"MNC_CONSECUTIVE_FAILURES",
				  std::to_string(health_probe_failures_)},
				 {"MNC_SPI_ERROR", std::to_string(health.spi_error)},
				 {"MNC_SPI_PROTOCOL_ERRORS",
				  std::to_string(
					  health.spi_protocol_error_count)},
				 {"MNC_SPI_REGISTER",
				  std::to_string(
					  health.spi_last_failed_register)},
				 {"MNC_SPI_RECEIVED_HEADER",
				  std::to_string(
					  health.spi_last_received_header)}});
		}

		/*
		 * A single bad SPI audit must not replace a known-good cache.
		 * Publish the failure after confirmation, or immediately when no
		 * previous snapshot exists during startup.
		 */
		if (has_cached_health_)
			update_cached_operational_health(health);
		if (!has_cached_health_ ||
		    health_probe_failures_ >= health_failures_before_degraded) {
			cached_health_ = health;
			has_cached_health_ = true;
			last_rpu_health_time_ = Clock::now();
			observe_rpu_health(health);
		}
	}

	void observe_rpu_health(const msap1_adc_health_payload &health)
	{
		if (last_health_flags_ &&
		    *last_health_flags_ == health.health_flags &&
		    last_spi_error_ == health.spi_error)
			return;
		const auto reasons = msap1::evaluate_rpu_adc_health_reasons(health);
		const bool healthy = reasons.empty();
		const auto reason_codes = health_reason_codes(reasons);
		const auto reason_messages = health_reason_messages(reasons);
		log_message(health_log,
			healthy ? mnc::logging::Priority::notice
				: mnc::logging::Priority::warning,
			healthy ? "RPU ADC health became healthy"
				: "RPU ADC health became degraded: " +
					reason_messages,
			healthy ? "rpu_health_healthy" : "rpu_health_degraded",
			{{"MNC_ADC_HEALTH_FLAGS",
			  std::to_string(health.health_flags)},
			 {"MNC_SPI_ERROR", std::to_string(health.spi_error)},
			 {"MNC_SPI_PROTOCOL_ERRORS",
			  std::to_string(health.spi_protocol_error_count)},
			 {"MNC_SPI_RETRY_RECOVERIES",
			  std::to_string(health.spi_retry_recovery_count)},
			 {"MNC_SPI_REGISTER",
			  std::to_string(health.spi_last_failed_register)},
			 {"MNC_SPI_RECEIVED_HEADER",
			  std::to_string(health.spi_last_received_header)},
			 {"MNC_HEALTH_REASONS", reason_codes},
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
		    acknowledgement.adc_source !=
			configuration_.wire.adc_source ||
		    (configuration_.wire.adc_source ==
			     MSAP1_ADC_SOURCE_SIMULATOR &&
		     acknowledgement.simulator_active_generation !=
			     configuration_.wire.generation) ||
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

	void apply_complete_configuration(
		msap1::PreparedMeterConfiguration staged,
		std::string_view event)
	{
		const auto previous = configuration_;
		const bool restart = running_;
		if (restart)
			stop();
		configuration_ = std::move(staged);
		latest_record_.reset();
		last_record_time_.reset();
		try {
			if (restart)
				start();
			else
				configure_meter();
			log_message(config_log, mnc::logging::Priority::notice,
				"ADC configuration applied: source=" +
					configuration_.source.adc_source,
				event,
				{{"MNC_CONFIGURATION_GENERATION",
				  std::to_string(configuration_.wire.generation)},
				 {"MNC_ADC_SOURCE",
				  configuration_.source.adc_source}});
		} catch (...) {
			if (running_)
				stop();
			configuration_ = previous;
			latest_record_.reset();
			last_record_time_.reset();
			try {
				if (restart)
					start();
				else
					configure_meter();
			} catch (const std::exception &rollback_error) {
				log_message(config_log,
					mnc::logging::Priority::critical,
					"ADC configuration rollback failed: " +
						std::string(rollback_error.what()),
					"adc_configuration_rollback_failed");
			}
			throw;
		}
	}

	void start(bool apply_configuration = true)
	{
		if (running_)
			return;
		/*
		 * Every deliberate DMA/capture restart starts a new continuity epoch.
		 * Source selection and other coordinated configurations may reset or
		 * advance PL record sequences while DMA is stopped; that boundary is
		 * not packet loss and must not increment the health gap counter.
		 */
		latest_record_.reset();
		last_record_time_.reset();
		sequence_gaps_ = 0;
		try {
			/*
			 * Both DMA consumers must own their S2MM channels before the
			 * RPU enables capture. This prevents losing the first waveform
			 * history block after a restart.
			 */
			waveform_.start();
			meter_.start();
			log_message(dma_log, mnc::logging::Priority::info,
				"meter DMA device opened: " +
					std::string(meter_.name()),
				"dma_opened",
				{{"MNC_DEVICE", std::string(meter_.name())}});
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
		} catch (...) {
			meter_.stop();
			log_message(dma_log, mnc::logging::Priority::info,
				"meter DMA device closed: " +
					std::string(meter_.name()),
				"dma_closed",
				{{"MNC_DEVICE", std::string(meter_.name())}});
			waveform_.stop();
			throw;
		}
		running_ = true;
		health_probe_failures_ = 0;
		health_stabilizing_ = true;
		next_health_audit_ =
			Clock::now() + health_startup_settle_interval;
		log_message(lifecycle_log, mnc::logging::Priority::notice,
			"ADC capture, meter DMA, and waveform DMA started",
			"capture_started",
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
		log_message(dma_log, mnc::logging::Priority::info,
			"meter DMA device closed: " + std::string(meter_.name()),
			"dma_closed",
			{{"MNC_DEVICE", std::string(meter_.name())}});
		waveform_.stop();
		running_ = false;
		health_stabilizing_ = false;
		log_message(lifecycle_log, mnc::logging::Priority::notice,
			"ADC capture, meter DMA, and waveform DMA stopped",
			"capture_stopped");
	}

	/** Applies one complete settings snapshot as a coordinated pipeline update. */
	void apply_product_settings(std::string_view json)
	{
		auto candidate = msap1::settings::SettingsCodec::decode(json);
		msap1::settings::SettingsValidator::validate(candidate);
		auto meter_settings =
			msap1::settings::to_meter_configuration(candidate);
		const bool pipeline_changed =
			candidate.metering.sample_rate_hz !=
				configuration_.wire.sample_rate_hz ||
			msap1::encode_meter_configuration(meter_settings, false) !=
				msap1::encode_meter_configuration(
					configuration_.source, false);
		if (pipeline_changed) {
			auto staged = msap1::prepare_meter_configuration(
				std::move(meter_settings),
				candidate.metering.sample_rate_hz);
			apply_complete_configuration(std::move(staged),
				"central_settings_applied");
		} else {
			log_message(config_log, mnc::logging::Priority::notice,
				"live service settings refreshed without restarting capture",
				"central_settings_live_applied");
		}
		product_settings_ = std::move(candidate);
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
		last_record_time_.reset();

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
				refresh_rpu_health();
			}
		} catch (...) {
			if (running_)
				stop();
			configuration_ = previous;
			latest_record_.reset();
			last_record_time_.reset();
			try {
				if (restart) {
					start();
				} else {
					configure_meter();
					refresh_rpu_health();
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
			if (!restart)
				refresh_rpu_health();
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
			const auto forward_distance = received - expected;
			if (forward_distance != 0u &&
			    forward_distance < (std::uint32_t{1} << 31u))
				sequence_gaps_ += forward_distance;
			else if (forward_distance != 0u) {
				/*
				 * A stale/out-of-order record is invalid, not billions of
				 * missing records. The half-range comparison keeps normal
				 * uint32 sequence wraparound valid.
				 */
				++invalid_records_;
				return;
			}
		}
		/* Durability is the publication boundary. A record is never made
		 * visible to web/CLI/publisher consumers until SQLite has committed
		 * the exact 256-byte PL record to the ordered WAL stream. */
		const auto received_at = std::chrono::system_clock::now();
		const auto cursor = record_stream_.append(record, received_at);
		const auto update = decoders_.decode(record, received_at);
		meter_data_.apply(update);
		latest_record_ = record;
		last_record_time_ = Clock::now();
		++meter_records_;
		(void)cursor;
	}

	void read_meter_records()
	{
		msap1::acquisition::MeterRecordBatch batch{};
		try {
			batch = meter_.read_available();
		} catch (const std::exception &error) {
			++dma_read_errors_;
			log_message(dma_log, mnc::logging::Priority::error,
				"meter DMA read failed: " + std::string(error.what()),
				"dma_read_failed");
			return;
		}
		if (batch.bytes == 0)
			return;
		dma_bytes_ += batch.bytes;
		if (batch.partial_record) {
			++invalid_records_;
			log_message(dma_log, mnc::logging::Priority::warning,
				"meter DMA returned a partial record",
				"dma_partial_record",
				{{"MNC_DMA_BYTES", std::to_string(batch.bytes)}});
			return;
		}
		for (std::size_t index = 0; index < batch.count; ++index)
			accept_record(batch.records[index]);
	}

	static std::uint32_t age_milliseconds(
		const std::optional<Clock::time_point> &timestamp)
	{
		if (!timestamp)
			return msap1::acquisition_age_unavailable;
		const auto age =
			std::chrono::duration_cast<std::chrono::milliseconds>(
				Clock::now() - *timestamp)
				.count();
		return static_cast<std::uint32_t>(std::clamp<std::int64_t>(
			age, 0, std::numeric_limits<std::uint32_t>::max()));
	}

	msap1::InfoResponse info_response()
	{
		msap1::InfoResponse response{};
		response.running = running_;
		response.has_meter_record = latest_record_.has_value();
		response.health_probe_pending =
			health_stabilizing_ || health_probe_failures_ != 0u;
		response.sample_rate_hz = configuration_.wire.sample_rate_hz;
		response.configuration_generation = configuration_.wire.generation;
		response.meter_record_age_ms = age_milliseconds(last_record_time_);
		response.rpu_health_age_ms =
			age_milliseconds(last_rpu_health_time_);
		response.health_probe_failures = health_probe_failures_;
		response.adc_source = configuration_.wire.adc_source;
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

	msap1::CaptureResponse capture_response() const
	{
		return {msap1::AcquisitionStatus::ok, running_};
	}

	msap1::FrequencyResponse frequency_response() const
	{
		return {msap1::AcquisitionStatus::ok,
			configuration_.wire.generation,
			frequency_ipc(configuration_.source.frequency)};
	}

	msap1::DiagnosticResponse diagnostic_response() const
	{
		msap1::DiagnosticResponse response{};
		response.running = running_;
		response.live_drdy_frequency_hz = cached_health_.drdy_frequency_hz;
		response.diagnostic = last_adc_diagnostic_;
		return response;
	}

	msap1::WaveformResponse waveform_response()
	{
		msap1::WaveformResponse response{};
		response.waveform = waveform_.status();
		for (const auto &session : waveform_.sessions())
			response.sessions.push_back(
				{session.id, session.trigger_sequence,
				 session.first_sequence, session.last_sequence,
				 session.trigger_tai_nanoseconds,
				 session.trigger_realtime_nanoseconds,
				 session.sample_rate_hz, session.event_count,
				 session.state,
				 std::string(session.filename.data())});
		return response;
	}

	msap1::AdcSourceResponse adc_source_response() const
	{
		msap1::AdcSourceResponse response{};
		response.running = running_;
		response.adc_source = configuration_.wire.adc_source;
		response.configuration_generation = configuration_.wire.generation;
		response.health_flags = cached_health_.health_flags;
		return response;
	}

	msap1::SimulatorResponse simulator_response() const
	{
		msap1::SimulatorResponse response{};
		response.adc_source = configuration_.wire.adc_source;
		response.configuration_generation = configuration_.wire.generation;
		response.health_flags = cached_health_.health_flags;
		response.simulator_active_generation =
			cached_health_.simulator_active_generation;
		response.simulator_frame_count =
			cached_health_.simulator_frame_count;
		response.simulator_saturation_count =
			cached_health_.simulator_saturation_count;
		response.simulator_missed_sample_count =
			cached_health_.simulator_missed_sample_count;
		response.simulator =
			simulator_ipc(configuration_.source.simulator);
		return response;
	}

	void configure_registry()
	{
		using msap1::AcquisitionStatus;
		registry_.set_error_observer(
			[](std::string_view command, std::string_view what) {
				log_message(lifecycle_log,
					mnc::logging::Priority::error,
					"acquisition command failed: " +
						std::string(what),
					"command_failed",
					{{"MNC_COMMAND", std::string(command)}});
			});
		registry_.on<msap1::InfoRequest>(AcquisitionStatus::dma_error,
			[this](const auto &) { return info_response(); });
		// The public health path returns the daemon cache. Web polling
		// must never trigger a 100-register SPI audit.
		registry_.on<msap1::HealthRequest>(AcquisitionStatus::dma_error,
			[this](const auto &) { return info_response(); });
		registry_.on<msap1::HealthRefreshRequest>(
			AcquisitionStatus::rpu_error, [this](const auto &) {
				refresh_rpu_health();
				return info_response();
			});
		registry_.on<msap1::StartRequest>(
			AcquisitionStatus::configuration_error,
			[this](const auto &) {
				start();
				return capture_response();
			});
		registry_.on<msap1::StopRequest>(AcquisitionStatus::dma_error,
			[this](const auto &) {
				stop();
				return capture_response();
			});
		registry_.on<msap1::FrequencyGetRequest>(
			AcquisitionStatus::dma_error,
			[this](const auto &) { return frequency_response(); });
		registry_.on<msap1::SampleRateSetRequest>(
			AcquisitionStatus::configuration_error,
			[this](const msap1::SampleRateSetRequest &request) {
				apply_sample_rate(request.sample_rate_hz);
				return info_response();
			});
		registry_.on<msap1::DiagnosticRunRequest>(
			AcquisitionStatus::configuration_error,
			[this](const msap1::DiagnosticRunRequest &request) {
				run_adc_diagnostic(request.flow);
				return diagnostic_response();
			});
		registry_.on<msap1::WaveformStatusRequest>(
			AcquisitionStatus::dma_error,
			[this](const auto &) { return waveform_response(); });
		registry_.on<msap1::WaveformListRequest>(
			AcquisitionStatus::dma_error,
			[this](const auto &) { return waveform_response(); });
		registry_.on<msap1::WaveformTriggerRequest>(
			AcquisitionStatus::dma_error,
			[this](const msap1::WaveformTriggerRequest &request) {
				const auto pretrigger_ms =
					request.pretrigger_ms ==
						msap1::waveform_duration_unspecified
					? product_settings_.waveform.default_pretrigger_ms
					: request.pretrigger_ms;
				const auto posttrigger_ms =
					request.posttrigger_ms ==
						msap1::waveform_duration_unspecified
					? product_settings_.waveform.default_posttrigger_ms
					: request.posttrigger_ms;
				const auto session = waveform_.trigger(
					pretrigger_ms, posttrigger_ms,
					request.source);
				log_message(waveform_log,
					mnc::logging::Priority::notice,
					"waveform capture triggered",
					"waveform_triggered",
					{{"MNC_WAVEFORM_SESSION",
					  std::to_string(session.id)},
					 {"MNC_PRETRIGGER_MS",
					  std::to_string(pretrigger_ms)},
					 {"MNC_POSTTRIGGER_MS",
					  std::to_string(posttrigger_ms)}});
				return waveform_response();
			});
		registry_.on<msap1::WaveformDeleteRequest>(
			AcquisitionStatus::dma_error,
			[this](const msap1::WaveformDeleteRequest &request) {
				waveform_.erase(request.session_id);
				log_message(waveform_log,
					mnc::logging::Priority::notice,
					"waveform capture deleted",
					"waveform_deleted",
					{{"MNC_WAVEFORM_SESSION",
					  std::to_string(request.session_id)}});
				return waveform_response();
			});
		registry_.on<msap1::AdcSourceGetRequest>(
			AcquisitionStatus::dma_error,
			[this](const auto &) { return adc_source_response(); });
		registry_.on<msap1::SimulatorGetRequest>(
			AcquisitionStatus::dma_error,
			[this](const auto &) { return simulator_response(); });
		registry_.on<msap1::ConfigurationApplyRequest>(
			AcquisitionStatus::configuration_error,
			[this](const msap1::ConfigurationApplyRequest &request) {
				apply_product_settings(request.configuration_json);
				return msap1::ApplyResponse{
					msap1::AcquisitionStatus::ok,
					configuration_.wire.generation};
			});
	}

	void drain_ipc_requests()
	{
		ipc_event_.consume();
		std::deque<PendingIpcRequest> requests;
		{
			std::scoped_lock lock(ipc_mutex_);
			requests.swap(ipc_requests_);
		}
		for (auto &pending : requests)
			pending.connection->post_send(
				registry_.dispatch(pending.frame));
	}

	Options options_;
	msap1::settings::ProductSettings product_settings_;
	msap1::PreparedMeterConfiguration configuration_;
	msap1::acquisition::MeterDmaReader meter_;
	msap1::WaveformCapture waveform_;
	msap1::acquisition::RpuController rpu_;
	msap1::MeterRecordStream record_stream_;
	msap1::MeterDecoderRegistry decoders_ =
		msap1::MeterDecoderRegistry::with_builtin_decoders();
	msap1::MeterData meter_data_;
	boost::asio::io_context ipc_context_;
	std::unique_ptr<mnc::ipc::UnixStreamServer> ipc_server_;
	std::thread ipc_thread_;
	EventSignal ipc_event_;
	std::mutex ipc_mutex_;
	std::deque<PendingIpcRequest> ipc_requests_;
	msap1::AcquisitionCommandRegistry registry_;
	bool running_ = false;
	std::uint64_t meter_records_ = 0;
	std::uint64_t dma_bytes_ = 0;
	std::uint64_t dma_read_errors_ = 0;
	std::uint64_t invalid_records_ = 0;
	std::uint64_t sequence_gaps_ = 0;
	std::optional<msap1::MeterRecord> latest_record_;
	std::optional<Clock::time_point> last_record_time_;
	msap1_adc_health_payload cached_health_{};
	msap1_adc_diagnostic_payload last_adc_diagnostic_{};
	bool has_cached_health_ = false;
	std::optional<Clock::time_point> last_rpu_health_time_;
	Clock::time_point next_health_audit_ = Clock::now();
	bool health_stabilizing_ = false;
	std::uint32_t health_probe_failures_ = 0;
	std::uint32_t last_spi_retry_recovery_count_ = 0;
	std::optional<std::uint32_t> last_health_flags_;
	std::uint32_t last_spi_error_ = 0;
};

class AcquisitionService final : public mnc::Service {
public:
	explicit AcquisitionService(const Options &options)
		: Service("MSAP1 FPGA acquisition", "fpga-acquisition"),
		  coordinator_(options)
	{
	}

protected:
	void on_start() override
	{
		acquisition_stop_requested = 0;
		worker_ = std::thread([this] {
			try {
				coordinator_.run();
			} catch (...) {
				failure_ = std::current_exception();
				failed_ = true;
				request_stop();
			}
		});
	}

	void on_reload() override
	{
		(void)logger().write(mnc::logging::Priority::info,
			"acquisition configuration reload requested; runtime "
			"configuration remains transaction-controlled",
			"reload_deferred");
	}

	void on_stop() noexcept override
	{
		acquisition_stop_requested = 1;
		if (worker_.joinable())
			worker_.join();
		if (!failure_)
			return;
		try {
			std::rethrow_exception(failure_);
		} catch (const std::exception &error) {
			(void)logger().write(mnc::logging::Priority::critical,
				"acquisition worker failed: " + std::string(error.what()),
				"worker_failed");
		} catch (...) {
			(void)logger().write(mnc::logging::Priority::critical,
				"acquisition worker failed", "worker_failed");
		}
	}

	[[nodiscard]] mnc::ServiceHealth health() const override
	{
		return failed_.load()
			? mnc::ServiceHealth{false, "acquisition worker failed"}
			: mnc::ServiceHealth{true, "acquisition running"};
	}

private:
	CaptureCoordinator coordinator_;
	std::thread worker_;
	std::exception_ptr failure_;
	std::atomic<bool> failed_{false};
};

} // namespace

int main(int argc, char **argv)
{
	try {
		AcquisitionService service(parse_options(argc, argv));
		return service.execute();
	} catch (const std::exception &error) {
		log_message(lifecycle_log, mnc::logging::Priority::critical,
			"msap1-fpga-acquisition: " + std::string(error.what()),
			"service_failed");
		return 1;
	}
}
