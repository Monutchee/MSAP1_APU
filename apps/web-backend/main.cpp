#include "msap1_auth_provider.hpp"
#include "systemd_notifier.hpp"

#include "msap1/acquisition_ipc.hpp"
#include "msap1/meter_health.hpp"
#include "msap1/soc_temperature.hpp"
#include "msap1/system_identity.hpp"
#include "mnc/logging/journal_reader.hpp"
#include "mnc/logging/logging.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glaze/glaze.hpp>
#include <webengine/NginxController.hpp>
#include <webengine/WebEngine.hpp>

namespace {

using namespace std::chrono_literals;

constexpr const char *web_socket_path = "/run/monutchee/web-backend.sock";
constexpr const char *nginx_config_path = "/etc/monutchee/msap1/nginx.conf";
constexpr const char *nginx_listen_path = "/run/monutchee/nginx/listen.conf";
constexpr const char *nginx_pid_path = "/run/monutchee/nginx/nginx.pid";
constexpr const char *nginx_temp_path = "/run/monutchee/nginx";

const mnc::logging::Logger lifecycle_log("web-backend", "lifecycle");
const mnc::logging::Logger nginx_log("web-backend", "nginx");
const mnc::logging::Logger api_log("web-backend", "http");

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

std::string request_id()
{
	static std::atomic<std::uint64_t> sequence{0};
	return std::to_string(++sequence);
}

void log_api_failure(std::string_view route, const std::exception &error,
		     const std::source_location &source =
			     std::source_location::current())
{
	log_message(api_log, mnc::logging::Priority::warning,
		"API request failed for " + std::string(route) + ": " +
			error.what(),
		"api_request_failed",
		{{"MNC_HTTP_ROUTE", std::string(route)}}, source);
}

struct SessionDto {
	std::string username;
	std::string role;
};

struct AcquisitionHealthDto {
	bool running;
	bool record_available;
	bool record_stale;
	std::uint32_t record_age_ms;
	std::uint32_t rpu_health_age_ms;
	std::uint32_t health_probe_failures;
	bool health_probe_pending;
	std::uint64_t records;
	std::uint64_t bytes;
	std::uint64_t read_errors;
	std::uint64_t invalid_records;
	std::uint64_t sequence_gaps;
	std::uint32_t configuration_generation;
};

struct HealthReasonDto {
	std::string code;
	std::string message;
};

struct AdcHealthDto {
	bool healthy;
	bool spi_responsive;
	bool initialized;
	bool init_complete;
	bool configuration_match;
	bool rate_match;
	bool capture_active;
	bool fifo_ok;
	bool headers_valid;
	bool meter_configured;
	bool meter_generation_match;
	bool dc_offset_removal;
	std::uint32_t sample_rate_hz;
	std::uint32_t frames;
	std::uint32_t packets;
	std::uint32_t dclk_frequency_hz;
	std::uint32_t drdy_frequency_hz;
	std::uint32_t fifo_overflows;
	std::uint32_t header_errors;
	std::uint32_t spi_protocol_errors;
	std::uint32_t spi_retry_recoveries;
	std::uint32_t spi_last_failed_register;
	std::uint32_t spi_last_received_header;
	std::vector<HealthReasonDto> degraded_reasons;
};

struct HealthDto {
	bool healthy;
	AcquisitionHealthDto acquisition;
	AdcHealthDto adc;
	bool frequency_arithmetic_ok;
	bool backend_running;
	bool nginx_running;
};

struct MeterHealthDto {
	bool healthy;
	AcquisitionHealthDto acquisition;
	AdcHealthDto adc;
	bool frequency_arithmetic_ok;
};

struct AdcCaptureDto {
	bool active;
};

struct MeterChannelDto {
	std::uint32_t index;
	std::string name;
	std::string unit;
	bool valid;
	std::int64_t mean_micro_units;
	std::uint32_t rms_count;
	double rms;
};

struct FrequencyReadingDto {
	bool enabled;
	bool valid;
	bool reference_valid;
	bool out_of_range;
	bool timed_out;
	bool arithmetic_error;
	double hz;
	std::uint32_t millihz;
	std::uint32_t period_q16_samples;
	std::uint32_t measurement_sequence;
	std::uint32_t mode;
	std::uint32_t reference_channel;
	std::uint32_t cycles_used;
};

struct FrequencyConfigurationDto {
	bool enabled = true;
	std::uint32_t reference_channel = 6;
	std::string mode = "rolling_cycles";
	std::uint32_t averaging_cycles = 10;
	std::uint32_t averaging_window_ms = 1000;
	double minimum_hz = 40.0;
	double maximum_hz = 70.0;
	double hysteresis_volts = 1.0;
};

struct MeterReadingsDto {
	std::uint32_t sequence;
	std::uint32_t configuration_generation;
	std::uint32_t sample_rate_hz;
	std::uint32_t rms_window_samples;
	std::uint32_t status;
	std::uint32_t capture_frames;
	std::uint32_t header_errors;
	std::uint32_t fifo_overflows;
	std::uint32_t packetizer_drops;
	std::uint32_t hub_drops;
	FrequencyReadingDto frequency;
	std::array<MeterChannelDto, 8> channels;
};

struct DeveloperLogEntryDto {
	std::int64_t timestamp_usec;
	std::string cursor;
	std::string priority;
	std::string message;
	std::string component;
	std::string module;
	std::string event;
	std::string request_id;
	std::string configuration_generation;
	std::string unit;
	std::string executable;
	std::string source_file;
	std::string source_line;
	std::string source_function;
	std::string raw;
};

struct DeveloperLogsDto {
	std::vector<DeveloperLogEntryDto> entries;
	std::string next_cursor;
};

struct SocTemperatureDto {
	std::string zone;
	std::string label;
	bool available;
	std::int64_t millidegrees_c;
	double temperature_c;
};

struct SocTemperaturesDto {
	std::int64_t sampled_at_unix_ms;
	std::vector<SocTemperatureDto> sensors;
};

struct SystemAboutDto {
	bool available;
	std::string product;
	std::string operating_system;
	std::string yocto_system_version;
	std::string build_hex;
	std::string software_build_date;
	std::string image_recipe;
	std::string machine;
};

struct ComponentFingerprintDto {
	std::string id;
	std::string label;
	std::string component_type;
	std::string path;
	bool available;
	std::uintmax_t size_bytes;
	std::string md5;
};

struct DeveloperAboutDto {
	std::string digest_algorithm;
	std::string digest_purpose;
	std::vector<ComponentFingerprintDto> components;
};

struct WaveformTriggerDto {
	std::uint32_t pretrigger_ms = 10000;
	std::uint32_t posttrigger_ms = 10000;
};

struct WaveformSessionDto {
	std::uint64_t id;
	std::string state;
	std::uint64_t trigger_sequence;
	std::uint64_t first_sequence;
	std::uint64_t last_sequence;
	std::uint64_t trigger_tai_nanoseconds;
	std::uint32_t sample_rate_hz;
	std::uint32_t event_count;
	std::string filename;
};

struct WaveformDto {
	bool running;
	bool active_session;
	std::uint32_t sample_rate_hz;
	std::uint32_t transport_ring_blocks;
	std::uint64_t blocks;
	std::uint64_t frames;
	std::uint64_t bytes;
	std::uint64_t invalid_blocks;
	std::uint64_t sequence_gaps;
	std::uint64_t transport_overrun_blocks;
	std::uint64_t materialization_failures;
	std::uint64_t history_oldest_sequence;
	std::uint64_t history_latest_sequence;
	std::uint64_t history_capacity_frames;
	std::uint64_t completed_sessions;
	std::uint64_t incomplete_sessions;
	std::vector<WaveformSessionDto> sessions;
};

template <typename T>
webengine::Response json_response(webengine::http::status status,
				  const T &value)
{
	auto body = glz::write_json(value);
	if (!body)
		return webengine::json(webengine::http::status::internal_server_error,
			R"({"error":"JSON serialization failed"})");
	return webengine::json(status, std::move(*body));
}

webengine::Response error_response(webengine::http::status status,
				   std::string message)
{
	return json_response(status, glz::obj{"error", std::move(message)});
}

unsigned hex_digit(char value)
{
	if (value >= '0' && value <= '9')
		return static_cast<unsigned>(value - '0');
	if (value >= 'a' && value <= 'f')
		return static_cast<unsigned>(value - 'a' + 10);
	if (value >= 'A' && value <= 'F')
		return static_cast<unsigned>(value - 'A' + 10);
	throw std::invalid_argument("invalid URL encoding");
}

std::string url_decode(std::string_view value)
{
	std::string result;
	result.reserve(value.size());
	for (std::size_t index = 0; index < value.size(); ++index) {
		if (value[index] == '+') {
			result.push_back(' ');
			continue;
		}
		if (value[index] != '%') {
			result.push_back(value[index]);
			continue;
		}
		if (index + 2 >= value.size())
			throw std::invalid_argument("invalid URL encoding");
		const auto byte = (hex_digit(value[index + 1]) << 4u) |
				  hex_digit(value[index + 2]);
		result.push_back(static_cast<char>(byte));
		index += 2;
	}
	return result;
}

std::unordered_map<std::string, std::string>
query_parameters(std::string_view target)
{
	std::unordered_map<std::string, std::string> result;
	const auto question = target.find('?');
	if (question == std::string_view::npos)
		return result;
	auto query = target.substr(question + 1);
	while (!query.empty()) {
		const auto separator = query.find('&');
		const auto item = query.substr(0, separator);
		const auto equals = item.find('=');
		const auto name = url_decode(item.substr(0, equals));
		const auto value = equals == std::string_view::npos
			? std::string{}
			: url_decode(item.substr(equals + 1));
		if (!name.empty())
			result.insert_or_assign(name, value);
		if (separator == std::string_view::npos)
			break;
		query.remove_prefix(separator + 1);
	}
	return result;
}

std::size_t log_limit(const std::unordered_map<std::string, std::string> &params)
{
	const auto item = params.find("limit");
	if (item == params.end())
		return 100;
	std::size_t value = 0;
	const auto parsed = std::from_chars(
		item->second.data(), item->second.data() + item->second.size(), value);
	if (parsed.ec != std::errc{} ||
	    parsed.ptr != item->second.data() + item->second.size() ||
	    value == 0 || value > 500)
		throw std::invalid_argument("log limit must be between 1 and 500");
	return value;
}

std::pair<std::string, std::string>
classify_log_entry(const mnc::logging::Entry &entry)
{
	if (!entry.component.empty())
		return {entry.component, entry.module};
	if (entry.unit == "dfx-mgr-fw-load.service")
		return {"firmware", "pl"};
	if (entry.unit == "msap1-dfx-firmware-rpu-load.service")
		return {"firmware", "rpu"};
	if (entry.unit == "msap1-fpga-acquisition.service")
		return {"fpga-acquisition", entry.module};
	if (entry.unit == "msap1-web-backend.service")
		return {"web-backend", entry.module};
	return {{}, {}};
}

DeveloperLogsDto developer_logs(std::string_view target)
{
	const auto params = query_parameters(target);
	mnc::logging::Query query;
	query.limit = log_limit(params);
	query.components = {"fpga-acquisition", "web-backend", "firmware"};
	query.units = {
		"msap1-fpga-acquisition.service",
		"msap1-web-backend.service",
		"dfx-mgr-fw-load.service",
		"msap1-dfx-firmware-rpu-load.service",
	};

	if (const auto item = params.find("component");
	    item != params.end() && !item->second.empty()) {
		if (item->second == "firmware") {
			query.components = {"firmware"};
			query.units = {
				"dfx-mgr-fw-load.service",
				"msap1-dfx-firmware-rpu-load.service",
			};
		} else if (item->second == "fpga-acquisition") {
			query.components = {"fpga-acquisition"};
			query.units = {"msap1-fpga-acquisition.service"};
		} else if (item->second == "web-backend") {
			query.components = {"web-backend"};
			query.units = {"msap1-web-backend.service"};
		} else {
			throw std::invalid_argument("unsupported log component");
		}
	}
	if (const auto item = params.find("module");
	    item != params.end() && !item->second.empty()) {
		/*
		 * Firmware lifecycle entries emitted by PID 1 have a UNIT but no
		 * MNC_MODULE. Translate the two synthetic firmware modules to unit
		 * filters before asking the generic journal reader to match them.
		 */
		if (item->second == "pl" || item->second == "rpu") {
			const auto component = params.find("component");
			if (component != params.end() && !component->second.empty() &&
			    component->second != "firmware")
				throw std::invalid_argument(
					"PL/RPU modules require the firmware component");
			query.components = {"firmware"};
			query.units = {item->second == "pl"
				? "dfx-mgr-fw-load.service"
				: "msap1-dfx-firmware-rpu-load.service"};
		} else {
			query.module = item->second;
		}
	}
	if (const auto item = params.find("priority");
	    item != params.end() && !item->second.empty()) {
		mnc::logging::Priority priority;
		if (!mnc::logging::parse_priority(item->second, priority))
			throw std::invalid_argument("unsupported log priority");
		query.maximum_priority = priority;
	}
	if (const auto item = params.find("after");
	    item != params.end() && !item->second.empty())
		query.after = mnc::logging::Cursor{item->second};

	mnc::logging::JournalReader reader;
	if (!reader.available())
		throw std::runtime_error("system journal is unavailable");
	const auto entries = reader.read(query);
	DeveloperLogsDto result;
	result.entries.reserve(entries.size());
	if (query.after)
		result.next_cursor = query.after->value;
	for (const auto &entry : entries) {
		auto classified = entry;
		const auto [component, module] = classify_log_entry(entry);
		if (classified.component.empty())
			classified.component = component;
		if (classified.module.empty())
			classified.module = module;
		const auto usec =
			std::chrono::duration_cast<std::chrono::microseconds>(
				classified.timestamp.time_since_epoch())
				.count();
		result.entries.push_back({
			usec,
			classified.cursor.value,
			mnc::logging::priority_name(classified.priority),
			classified.message,
			classified.component,
			classified.module,
			classified.event,
			classified.request_id,
			classified.configuration_generation,
			classified.unit,
			classified.executable,
			classified.source_file,
			classified.source_line,
			classified.source_function,
			mnc::logging::entry_to_json(classified),
		});
		result.next_cursor = classified.cursor.value;
	}
	return result;
}

SocTemperaturesDto soc_temperatures()
{
	const auto readings = msap1::read_soc_temperatures();
	SocTemperaturesDto result{
		std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch())
			.count(),
		{},
	};
	result.sensors.reserve(readings.size());
	for (const auto &reading : readings) {
		result.sensors.push_back({
			reading.zone,
			reading.label,
			reading.available(),
			reading.millidegrees_c.value_or(0),
			reading.celsius(),
		});
	}
	return result;
}

SystemAboutDto system_about()
{
	const auto identity = msap1::read_image_identity();
	auto short_hash = identity.build_hash_short;
	if (short_hash.empty() && identity.build_hash.size() >= 6)
		short_hash = identity.build_hash.substr(0, 6);
	return {
		identity.available,
		"MSAP1",
		"MNCOS",
		identity.distro_version,
		std::move(short_hash),
		identity.build_time,
		identity.image_recipe,
		identity.machine,
	};
}

DeveloperAboutDto developer_about()
{
	DeveloperAboutDto result{
		"MD5",
		"Diagnostic file identity only; MD5 is not an integrity or security check",
		{},
	};
	const auto fingerprints = msap1::system_component_fingerprints();
	result.components.reserve(fingerprints.size());
	for (const auto &fingerprint : fingerprints) {
		result.components.push_back({
			fingerprint.id,
			fingerprint.label,
			fingerprint.component_type,
			fingerprint.path,
			fingerprint.available,
			fingerprint.size_bytes,
			fingerprint.md5,
		});
	}
	return result;
}

std::string waveform_state_name(msap1::WaveformSessionState state)
{
	switch (state) {
	case msap1::WaveformSessionState::capturing: return "capturing";
	case msap1::WaveformSessionState::complete: return "complete";
	case msap1::WaveformSessionState::incomplete: return "incomplete";
	}
	return "unknown";
}

WaveformDto waveform_status(const msap1::AcquisitionResponse &response)
{
	const auto &status = response.waveform;
	WaveformDto result{
		status.running != 0u,
		status.active_session != 0u,
		status.sample_rate_hz,
		status.transport_ring_blocks,
		status.blocks,
		status.frames,
		status.bytes,
		status.invalid_blocks,
		status.sequence_gaps,
		status.transport_overrun_blocks,
		status.materialization_failures,
		status.history_oldest_sequence,
		status.history_latest_sequence,
		status.history_capacity_frames,
		status.completed_sessions,
		status.incomplete_sessions,
		{},
	};
	const auto count = std::min<std::size_t>(
		response.waveform_session_count,
		response.waveform_sessions.size());
	result.sessions.reserve(count);
	for (std::size_t index = 0; index < count; ++index) {
		const auto &session = response.waveform_sessions[index];
		result.sessions.push_back({
			session.id,
			waveform_state_name(session.state),
			session.trigger_sequence,
			session.first_sequence,
			session.last_sequence,
			session.trigger_tai_nanoseconds,
			session.sample_rate_hz,
			session.event_count,
			session.filename.data(),
		});
	}
	return result;
}

void require_acquisition_ok(const msap1::AcquisitionResponse &response)
{
	if (response.status != msap1::AcquisitionStatus::ok)
		throw std::runtime_error("acquisition daemon returned status " +
			std::to_string(static_cast<std::uint32_t>(response.status)));
}

MeterHealthDto meter_health(const msap1::AcquisitionResponse &response)
{
	const auto &adc = response.rpu_health;
	const auto status = msap1::evaluate_meter_health(response);
	std::vector<HealthReasonDto> degraded_reasons;
	degraded_reasons.reserve(status.adc_degraded_reasons.size());
	for (const auto &reason : status.adc_degraded_reasons)
		degraded_reasons.push_back({reason.code, reason.message});
	return {
		status.healthy,
		{response.running != 0u, response.has_meter_record != 0u,
		 status.record_stale, response.meter_record_age_ms,
		 response.rpu_health_age_ms, response.health_probe_failures,
		 response.health_probe_pending != 0u,
		 response.meter_records, response.dma_bytes, response.dma_read_errors,
		 response.invalid_records, response.sequence_gaps,
		 response.configuration_generation},
		{status.adc_healthy, status.spi_responsive, status.initialized,
		 (adc.health_flags & MSAP1_ADC_HEALTH_INIT_COMPLETE) != 0u,
		 status.configuration_match, status.rate_match,
		 status.capture_active, status.fifo_ok, status.headers_valid,
		 status.meter_configured,
		 status.meter_generation_match, status.dc_offset_removal,
		 adc.sample_rate_hz, adc.frame_count, adc.packet_count,
		 adc.dclk_frequency_hz,
		 adc.drdy_frequency_hz,
		 adc.overflow_count,
		 adc.header_error_count,
		 adc.spi_protocol_error_count,
		 adc.spi_retry_recovery_count,
		 adc.spi_last_failed_register,
		 adc.spi_last_received_header,
		 std::move(degraded_reasons)},
		status.frequency_arithmetic_ok,
	};
}

HealthDto system_health(const msap1::AcquisitionResponse &response,
			const webengine::NginxController &nginx)
{
	const auto meter = meter_health(response);
	const bool nginx_ok = nginx.is_running();
	return {
		meter.healthy && nginx_ok,
		meter.acquisition,
		meter.adc,
		meter.frequency_arithmetic_ok,
		true,
		nginx_ok,
	};
}

MeterReadingsDto readings(const msap1::AcquisitionResponse &response)
{
	if (response.running == 0u || response.has_meter_record == 0u)
		throw std::runtime_error("no meter result is available");
	const auto &record = response.latest_record;
	if (!record.header_valid())
		throw std::runtime_error("meter record header is invalid");
	static constexpr std::array<const char *, 8> names{
		"ILA", "ILB", "ILC", "ILN", "VLC", "VLB", "VLA", "VCM"};
	const auto frequency = record.frequency();
	MeterReadingsDto result{
		record.sequence(), record.configuration_generation(),
		record.sample_rate_hz(), record.window_samples(), record.status(),
		record.capture_frames(), record.header_errors(),
		record.fifo_overflows(), record.packetizer_drops(), record.hub_drops(),
		{frequency.enabled, frequency.valid, frequency.reference_valid,
		 frequency.out_of_range, frequency.timed_out,
		 frequency.arithmetic_error,
		 frequency.valid ? static_cast<double>(frequency.millihz) / 1000.0
				 : 0.0,
		 frequency.millihz, frequency.period_q16_samples,
		 frequency.measurement_sequence, frequency.mode,
		 frequency.reference_channel, frequency.cycles_used},
		{},
	};
	for (std::size_t index = 0; index < result.channels.size(); ++index) {
		const auto reading = record.channel(index);
		result.channels[index] = {
			static_cast<std::uint32_t>(index), names[index],
			index >= 4 && index <= 6 ? "V" : "A", reading.valid,
			reading.mean_micro_units, reading.rms_count,
			reading.valid
				? static_cast<double>(reading.rms_micro_units) / 1000000.0
				: 0.0,
		};
	}
	return result;
}

FrequencyConfigurationDto frequency_configuration(
	const msap1::FrequencyIpcConfiguration &frequency)
{
	const char *mode = "rolling_cycles";
	if (frequency.mode == MSAP1_FREQUENCY_MODE_SINGLE_CYCLE)
		mode = "single_cycle";
	else if (frequency.mode == MSAP1_FREQUENCY_MODE_ROLLING_TIME)
		mode = "rolling_time";
	return {
		frequency.enabled != 0u,
		frequency.reference_channel,
		mode,
		frequency.averaging_cycles,
		frequency.averaging_window_ms,
		static_cast<double>(frequency.minimum_millihz) / 1000.0,
		static_cast<double>(frequency.maximum_millihz) / 1000.0,
		static_cast<double>(frequency.hysteresis_microvolts) / 1000000.0,
	};
}

msap1::FrequencyIpcConfiguration frequency_ipc(
	const FrequencyConfigurationDto &frequency)
{
	std::uint32_t mode;
	if (frequency.mode == "single_cycle")
		mode = MSAP1_FREQUENCY_MODE_SINGLE_CYCLE;
	else if (frequency.mode == "rolling_cycles")
		mode = MSAP1_FREQUENCY_MODE_ROLLING_CYCLES;
	else if (frequency.mode == "rolling_time")
		mode = MSAP1_FREQUENCY_MODE_ROLLING_TIME;
	else
		throw std::invalid_argument("unsupported frequency mode");

	if (!std::isfinite(frequency.minimum_hz) ||
	    !std::isfinite(frequency.maximum_hz) ||
	    !std::isfinite(frequency.hysteresis_volts) ||
	    frequency.reference_channel != 6u ||
	    frequency.averaging_cycles < 1u ||
	    frequency.averaging_cycles > 64u ||
	    frequency.averaging_window_ms < 100u ||
	    frequency.averaging_window_ms > 1000u ||
	    frequency.minimum_hz < 10.0 ||
	    frequency.maximum_hz > 200.0 ||
	    frequency.minimum_hz >= frequency.maximum_hz ||
	    frequency.hysteresis_volts <= 0.0 ||
	    frequency.hysteresis_volts > 100.0)
		throw std::invalid_argument("frequency configuration is out of range");
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

std::string getenv_or(const char *name, const char *fallback)
{
	const char *value = std::getenv(name);
	return value != nullptr && value[0] != '\0' ? value : fallback;
}

std::uint16_t web_port(const char *environment_name, const char *fallback)
{
	const auto value = getenv_or(environment_name, fallback);
	const auto port = std::stoul(value);
	if (port == 0 || port > std::numeric_limits<std::uint16_t>::max())
		throw std::invalid_argument(std::string(environment_name) +
			" is out of range");
	return static_cast<std::uint16_t>(port);
}

bool wait_for_socket(const std::filesystem::path &path,
		     const std::atomic<bool> &finished)
{
	const auto deadline = std::chrono::steady_clock::now() + 5s;
	while (std::chrono::steady_clock::now() < deadline && !finished) {
		std::error_code error;
		if (std::filesystem::exists(path, error))
			return true;
		std::this_thread::sleep_for(20ms);
	}
	return false;
}

} // namespace

int main()
{
	try {
		log_message(lifecycle_log, mnc::logging::Priority::notice,
			"MSAP1 web backend is starting", "service_starting");
		auto auth = std::make_shared<msap1::web::Msap1AuthProvider>();

		webengine::NginxController::Options nginx_options;
		nginx_options.config = getenv_or("MSAP1_NGINX_CONFIG", nginx_config_path);
		nginx_options.listen_file =
			getenv_or("MSAP1_NGINX_LISTEN_FILE", nginx_listen_path);
		nginx_options.pidfile = getenv_or("MSAP1_NGINX_PIDFILE", nginx_pid_path);
		nginx_options.temp_root =
			getenv_or("MSAP1_NGINX_TEMP_ROOT", nginx_temp_path);
		nginx_options.http_port = web_port("MSAP1_WEB_HTTP_PORT", "80");
		nginx_options.https_port = web_port("MSAP1_WEB_HTTPS_PORT", "443");
		nginx_options.https_enabled = true;
		webengine::NginxController nginx(std::move(nginx_options));

		webengine::WebEngine engine(auth);
		engine.set_socket_path(getenv_or("MSAP1_WEB_SOCKET", web_socket_path))
			.set_threads(2)
			.enable_signal_shutdown()
			.enable_auth_endpoints();

		engine.add_api(webengine::http::verb::get, "/api/v1/session",
			[](const webengine::RequestContext &context) {
				const auto &user = *context.user;
				return json_response(webengine::http::status::ok,
					SessionDto{user.username,
						   webengine::role_name(user.role)});
			}, webengine::Role::Viewer);

		engine.add_api(webengine::http::verb::get, "/api/v1/health",
			[&nginx](const webengine::RequestContext &) {
				try {
					msap1::AcquisitionClient client;
					const auto response = client.request(
						msap1::AcquisitionCommand::info, 1000);
					require_acquisition_ok(response);
					return json_response(webengine::http::status::ok,
						system_health(response, nginx));
				} catch (const std::exception &error) {
					log_api_failure("/api/v1/health", error);
					return error_response(
						webengine::http::status::service_unavailable,
						error.what());
				}
			}, webengine::Role::Viewer);

		engine.add_api(webengine::http::verb::get, "/api/v1/about",
			[](const webengine::RequestContext &) {
				return json_response(webengine::http::status::ok,
					system_about());
			}, webengine::Role::Viewer);

		engine.add_api(webengine::http::verb::get,
			"/api/v1/developer/temperatures",
			[](const webengine::RequestContext &) {
				try {
					return json_response(webengine::http::status::ok,
						soc_temperatures());
				} catch (const std::exception &error) {
					log_api_failure(
						"/api/v1/developer/temperatures", error);
					return error_response(
						webengine::http::status::service_unavailable,
						error.what());
				}
			}, webengine::Role::Admin);

		engine.add_api(webengine::http::verb::get,
			"/api/v1/developer/about",
			[](const webengine::RequestContext &) {
				return json_response(webengine::http::status::ok,
					developer_about());
			}, webengine::Role::Admin);

		engine.add_api(webengine::http::verb::get,
			"/api/v1/developer/logs",
			[](const webengine::RequestContext &context) {
				try {
					const auto target = context.request.target();
					return json_response(
						webengine::http::status::ok,
						developer_logs(std::string_view{
							target.data(), target.size()}));
				} catch (const std::invalid_argument &error) {
					return error_response(
						webengine::http::status::bad_request,
						error.what());
				} catch (const std::runtime_error &error) {
					if (std::string_view{error.what()} ==
					    "journal cursor is no longer valid")
						return error_response(
							webengine::http::status::conflict,
							error.what());
					log_api_failure("/api/v1/developer/logs", error);
					return error_response(
						webengine::http::status::service_unavailable,
						error.what());
				} catch (const std::exception &error) {
					log_api_failure("/api/v1/developer/logs", error);
					return error_response(
						webengine::http::status::service_unavailable,
						error.what());
				}
			}, webengine::Role::Admin);

		engine.add_api(webengine::http::verb::get,
			"/api/v1/meter/health",
			[](const webengine::RequestContext &) {
				try {
					msap1::AcquisitionClient client;
					const auto response = client.request(
						msap1::AcquisitionCommand::info, 1000);
					require_acquisition_ok(response);
					return json_response(webengine::http::status::ok,
						meter_health(response));
				} catch (const std::exception &error) {
					log_api_failure("/api/v1/meter/health", error);
					return error_response(
						webengine::http::status::service_unavailable,
						error.what());
				}
			}, webengine::Role::Viewer);

		engine.add_api(webengine::http::verb::get,
			"/api/v1/meter/readings",
			[](const webengine::RequestContext &) {
				try {
					msap1::AcquisitionClient client;
					const auto response = client.request(
						msap1::AcquisitionCommand::info, 1000);
					require_acquisition_ok(response);
					return json_response(webengine::http::status::ok,
						readings(response));
				} catch (const std::exception &error) {
					log_api_failure("/api/v1/meter/readings", error);
					return error_response(
						webengine::http::status::service_unavailable,
						error.what());
				}
			}, webengine::Role::Viewer);

		engine.add_api(webengine::http::verb::get,
			"/api/v1/waveforms",
			[](const webengine::RequestContext &) {
				try {
					msap1::AcquisitionClient client;
					const auto response = client.request(
						msap1::AcquisitionCommand::waveform_status,
						1000);
					require_acquisition_ok(response);
					return json_response(webengine::http::status::ok,
						waveform_status(response));
				} catch (const std::exception &error) {
					log_api_failure("/api/v1/waveforms", error);
					return error_response(
						webengine::http::status::service_unavailable,
						error.what());
				}
			}, webengine::Role::Viewer);

		engine.add_api(webengine::http::verb::post,
			"/api/v1/waveforms/trigger",
			[](const webengine::RequestContext &context) {
				const auto correlation = request_id();
				try {
					WaveformTriggerDto trigger;
					if (const auto error = glz::read_json(
						    trigger, context.request.body()))
						return error_response(
							webengine::http::status::bad_request,
							"invalid waveform trigger JSON");
					if (trigger.pretrigger_ms > 120000u ||
					    trigger.posttrigger_ms > 120000u)
						return error_response(
							webengine::http::status::bad_request,
							"waveform durations must be 0..120000 ms");
					msap1::AcquisitionClient client;
					const auto response = client.request(
						msap1::AcquisitionCommand::waveform_trigger,
						3000, nullptr, 0, 0,
						trigger.pretrigger_ms,
						trigger.posttrigger_ms,
						msap1::WaveformTriggerSource::manual_web);
					require_acquisition_ok(response);
					log_message(api_log,
						mnc::logging::Priority::notice,
						"manual waveform capture triggered",
						"waveform_triggered",
						{{"MNC_REQUEST_ID", correlation},
						 {"MNC_PRETRIGGER_MS",
						  std::to_string(trigger.pretrigger_ms)},
						 {"MNC_POSTTRIGGER_MS",
						  std::to_string(trigger.posttrigger_ms)}});
					return json_response(webengine::http::status::ok,
						waveform_status(response));
				} catch (const std::exception &error) {
					log_api_failure(
						"/api/v1/waveforms/trigger", error);
					return error_response(
						webengine::http::status::service_unavailable,
						error.what());
				}
			}, webengine::Role::Admin);

		engine.add_api(webengine::http::verb::get,
			"/api/v1/meter/configuration/frequency",
			[](const webengine::RequestContext &) {
				try {
					msap1::AcquisitionClient client;
					const auto response = client.request(
						msap1::AcquisitionCommand::
							frequency_configuration_get,
						1000);
					require_acquisition_ok(response);
					return json_response(webengine::http::status::ok,
						frequency_configuration(response.frequency));
				} catch (const std::exception &error) {
					log_api_failure(
						"/api/v1/meter/configuration/frequency",
						error);
					return error_response(
						webengine::http::status::service_unavailable,
						error.what());
				}
			}, webengine::Role::Viewer);

		engine.add_api(webengine::http::verb::put,
			"/api/v1/meter/configuration/frequency",
			[](const webengine::RequestContext &context) {
				const auto correlation = request_id();
				log_message(api_log, mnc::logging::Priority::info,
					"frequency configuration update requested",
					"frequency_update_requested",
					{{"MNC_REQUEST_ID", correlation}});
				try {
					FrequencyConfigurationDto configuration;
					if (const auto error = glz::read_json(
						    configuration, context.request.body())) {
						log_message(api_log,
							mnc::logging::Priority::warning,
							"frequency configuration JSON is invalid",
							"frequency_update_rejected",
							{{"MNC_REQUEST_ID",
							  correlation}});
						return error_response(
							webengine::http::status::bad_request,
							"invalid frequency configuration JSON");
					}
					const auto wire = frequency_ipc(configuration);
					msap1::AcquisitionClient client;
					const auto response = client.request(
						msap1::AcquisitionCommand::
							frequency_configuration_set,
						5000, &wire);
					if (response.status ==
					    msap1::AcquisitionStatus::configuration_error) {
						log_message(api_log,
							mnc::logging::Priority::warning,
							"frequency configuration was rejected",
							"frequency_update_rejected",
							{{"MNC_REQUEST_ID",
							  correlation}});
						return error_response(
							webengine::http::status::bad_request,
							"frequency configuration was rejected");
					}
					require_acquisition_ok(response);
					log_message(api_log,
						mnc::logging::Priority::notice,
						"frequency configuration update applied",
						"frequency_update_applied",
						{{"MNC_REQUEST_ID", correlation},
						 {"MNC_CONFIGURATION_GENERATION",
						  std::to_string(response
							.configuration_generation)}});
					return json_response(webengine::http::status::ok,
						frequency_configuration(response.frequency));
				} catch (const std::invalid_argument &error) {
					log_message(api_log,
						mnc::logging::Priority::warning,
						"frequency configuration rejected: " +
							std::string(error.what()),
						"frequency_update_rejected",
						{{"MNC_REQUEST_ID", correlation}});
					return error_response(
						webengine::http::status::bad_request,
						error.what());
				} catch (const std::exception &error) {
					log_message(api_log,
						mnc::logging::Priority::error,
						"frequency configuration failed: " +
							std::string(error.what()),
						"frequency_update_failed",
						{{"MNC_REQUEST_ID", correlation}});
					return error_response(
						webengine::http::status::service_unavailable,
						error.what());
				}
			}, webengine::Role::Admin);

		const auto capture = [](msap1::AcquisitionCommand command) {
			return [command](const webengine::RequestContext &) {
				const bool query =
					command == msap1::AcquisitionCommand::info;
				const auto correlation = request_id();
				const bool starting =
					command == msap1::AcquisitionCommand::start;
				if (!query)
					log_message(api_log,
						mnc::logging::Priority::info,
						starting
							? "ADC capture start requested"
							: "ADC capture stop requested",
						starting
							? "capture_start_requested"
							: "capture_stop_requested",
						{{"MNC_REQUEST_ID", correlation}});
				try {
					msap1::AcquisitionClient client;
					const auto response = client.request(command, 3000);
					require_acquisition_ok(response);
					if (!query)
						log_message(api_log,
							mnc::logging::Priority::notice,
							starting
								? "ADC capture started"
								: "ADC capture stopped",
							starting
								? "capture_start_applied"
								: "capture_stop_applied",
							{{"MNC_REQUEST_ID",
							  correlation}});
					return json_response(webengine::http::status::ok,
						AdcCaptureDto{response.running != 0u});
				} catch (const std::exception &error) {
					log_message(api_log,
						mnc::logging::Priority::error,
						std::string("ADC capture command failed: ") +
							error.what(),
						"capture_command_failed",
						{{"MNC_REQUEST_ID", correlation}});
					return error_response(
						webengine::http::status::service_unavailable,
						error.what());
				}
			};
		};
		engine.add_api(webengine::http::verb::get, "/api/v1/adc/capture",
			capture(msap1::AcquisitionCommand::info),
			webengine::Role::Viewer);
		engine.add_api(webengine::http::verb::put, "/api/v1/adc/capture",
			capture(msap1::AcquisitionCommand::start),
			webengine::Role::Admin);
		engine.add_api(webengine::http::verb::delete_, "/api/v1/adc/capture",
			capture(msap1::AcquisitionCommand::stop),
			webengine::Role::Admin);

		std::atomic<bool> engine_finished{false};
		std::exception_ptr engine_error;
		std::thread engine_thread([&] {
			try {
				engine.run();
			} catch (...) {
				engine_error = std::current_exception();
			}
			engine_finished = true;
		});

		const auto socket_path = getenv_or("MSAP1_WEB_SOCKET", web_socket_path);
		if (!wait_for_socket(socket_path, engine_finished)) {
			engine.stop();
			engine_thread.join();
			throw std::runtime_error("WebEngine did not create its Unix socket");
		}
		log_message(lifecycle_log, mnc::logging::Priority::info,
			"WebEngine Unix socket is ready: " + socket_path,
			"backend_socket_ready",
			{{"MNC_SOCKET_PATH", socket_path}});
		if (!nginx.on()) {
			engine.stop();
			engine_thread.join();
			throw std::runtime_error("nginx failed to start: " + nginx.last_error());
		}
		log_message(nginx_log, mnc::logging::Priority::notice,
			"nginx started under web-backend supervision",
			"nginx_started");

		msap1::web::SystemdNotifier notifier;
		(void)notifier.ready("MSAP1 web backend and nginx are ready");
		log_message(lifecycle_log, mnc::logging::Priority::notice,
			"MSAP1 web backend and nginx are ready", "service_ready");
		unsigned recovery_failures = 0;
		while (!engine_finished) {
			std::this_thread::sleep_for(5s);
			if (engine_finished)
				break;
			if (!nginx.is_running()) {
				log_message(nginx_log, mnc::logging::Priority::warning,
					"nginx stopped unexpectedly; recovery is starting",
					"nginx_unexpected_exit",
					{{"MNC_RECOVERY_ATTEMPT",
					  std::to_string(recovery_failures + 1)}});
				if (!nginx.on()) {
					if (++recovery_failures >= 3) {
						log_message(nginx_log,
							mnc::logging::Priority::critical,
							"nginx recovery failed repeatedly: " +
								nginx.last_error(),
							"nginx_recovery_exhausted");
						engine.stop();
						break;
					}
					continue;
				}
				recovery_failures = 0;
				log_message(nginx_log, mnc::logging::Priority::notice,
					"nginx recovered successfully",
					"nginx_recovered");
			}
			(void)notifier.watchdog("MSAP1 web backend and nginx are healthy");
		}

		(void)notifier.stopping("MSAP1 web backend is stopping");
		log_message(lifecycle_log, mnc::logging::Priority::notice,
			"MSAP1 web backend is stopping", "service_stopping");
		(void)nginx.off();
		log_message(nginx_log, mnc::logging::Priority::info,
			"nginx stopped", "nginx_stopped");
		engine.stop();
		engine_thread.join();
		if (engine_error)
			std::rethrow_exception(engine_error);
		if (recovery_failures >= 3)
			throw std::runtime_error("nginx recovery failed repeatedly: " +
				nginx.last_error());
		return 0;
	} catch (const std::exception &error) {
		log_message(lifecycle_log, mnc::logging::Priority::critical,
			"msap1-web-backend: " + std::string(error.what()),
			"service_failed");
		return 1;
	}
}
