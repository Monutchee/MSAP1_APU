/**
 * @file meter_routes.cpp
 * @brief Metering endpoints: pipeline health, latest readings, and the
 *        frequency measurement configuration.
 */

#include "health_dto.hpp"
#include "response.hpp"
#include "routes.hpp"

#include "msap1/meter/meter_health.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <glaze/glaze.hpp>

namespace msap1::web::api {

namespace {

/** One channel of the latest meter record. */
struct MeterChannelDto {
	std::uint32_t index;
	std::string name;
	std::string unit;
	bool valid;
	std::int64_t mean_micro_units;
	std::uint32_t rms_count;
	double rms;
};

/** Frequency measurement embedded in GET /api/v1/meter/readings. */
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

/** Body of GET/PUT /api/v1/meter/configuration/frequency. */
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

/** Body of GET /api/v1/meter/readings. */
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

/** Project the newest cached meter record onto the readings DTO. */
MeterReadingsDto readings(const msap1::InfoResponse &response)
{
	if (!response.running || !response.has_meter_record)
		throw std::runtime_error("no meter result is available");
	const auto &record = response.latest_record;
	if (!record.header_valid())
		throw std::runtime_error("meter record header is invalid");
	static constexpr std::array<const char *, 8> names{
		"ILA", "ILB", "ILC", "ILN", "VLC", "VLB", "VLA", "VCM"};
	const auto frequency = record.frequency();
	MeterReadingsDto result{
		record.sequence(), record.configuration_generation(),
		record.sample_rate_hz(), record.window_samples(),
		record.status(),
		record.capture_frames(), record.header_errors(),
		record.fifo_overflows(), record.packetizer_drops(),
		record.hub_drops(),
		{frequency.enabled, frequency.valid, frequency.reference_valid,
		 frequency.out_of_range, frequency.timed_out,
		 frequency.arithmetic_error,
		 frequency.valid
			 ? static_cast<double>(frequency.millihz) / 1000.0
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
				? static_cast<double>(reading.rms_micro_units) /
					  1000000.0
				: 0.0,
		};
	}
	return result;
}

/** Render the acquisition daemon's active frequency configuration. */
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
		static_cast<double>(frequency.hysteresis_microvolts) /
			1000000.0,
	};
}

/**
 * Validate a requested frequency configuration and convert it to the wire
 * representation.  This is the single range-validation authority.
 *
 * @throws std::invalid_argument when any value is out of range.
 */
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
		throw std::invalid_argument(
			"frequency configuration is out of range");
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

msap1::FrequencyConfig frequency_settings(
	const FrequencyConfigurationDto &frequency)
{
	/* Reuse the wire conversion as the single range-validation authority,
	 * then preserve the human-facing values in the persistent typed
	 * document. */
	(void)frequency_ipc(frequency);
	return {frequency.enabled, frequency.reference_channel, frequency.mode,
		frequency.averaging_cycles, frequency.averaging_window_ms,
		frequency.minimum_hz, frequency.maximum_hz,
		frequency.hysteresis_volts};
}

} // namespace

/**
 * @brief Project an acquisition InfoResponse onto the meter health DTO.
 *
 * Shared with health_routes.cpp, which embeds the same projection in the
 * aggregate GET /api/v1/health document.
 */
MeterHealthDto meter_health_dto(const msap1::InfoResponse &response)
{
	const auto adc = response.rpu_health.value();
	const auto status = msap1::evaluate_meter_health(response);
	std::vector<HealthReasonDto> degraded_reasons;
	degraded_reasons.reserve(status.adc_degraded_reasons.size());
	for (const auto &reason : status.adc_degraded_reasons)
		degraded_reasons.push_back({reason.code, reason.message});
	return {
		status.healthy,
		{response.running, response.has_meter_record,
		 status.record_stale, response.meter_record_age_ms,
		 response.rpu_health_age_ms, response.health_probe_failures,
		 response.health_probe_pending,
		 response.meter_records, response.dma_bytes,
		 response.dma_read_errors,
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
		 adc_source_name(adc.adc_source),
		 (adc.health_flags & MSAP1_ADC_HEALTH_PHYSICAL_DIAGNOSTICS) != 0u,
		 (adc.health_flags & MSAP1_ADC_HEALTH_SIMULATOR_HEALTHY) != 0u,
		 adc.simulator_active_generation,
		 adc.simulator_frame_count,
		 adc.simulator_saturation_count,
		 adc.simulator_missed_sample_count,
		 std::move(degraded_reasons)},
		status.frequency_arithmetic_ok,
	};
}

/**
 * @brief GET /api/v1/meter/health (Viewer)
 *
 * Reports the metering pipeline health: acquisition transport state,
 * meter-record freshness, and the cached RPU ADC audit with any degraded
 * reason codes.
 *
 * @return 200 with the meter health document, or 503 when the acquisition
 *         daemon is unreachable or reports a failure status.
 */
webengine::Response get_meter_health(AppContext &app,
				     const webengine::RequestContext &)
{
	try {
		const auto response = app.acquisition.information();
		require_acquisition_ok(response.status);
		return json_response(webengine::http::status::ok,
			meter_health_dto(response));
	} catch (const std::exception &error) {
		log_api_failure("/api/v1/meter/health", error);
		return error_response(
			webengine::http::status::service_unavailable,
			error.what());
	}
}

/**
 * @brief GET /api/v1/meter/readings (Viewer)
 *
 * Returns the newest coherent meter record: per-channel RMS and mean
 * values, capture statistics, and the frequency measurement.
 *
 * @return 200 with the readings document, or 503 when capture is stopped,
 *         no record is available yet, or the daemon is unreachable.
 */
webengine::Response get_meter_readings(AppContext &app,
				       const webengine::RequestContext &)
{
	try {
		const auto response = app.acquisition.information();
		require_acquisition_ok(response.status);
		return json_response(webengine::http::status::ok,
			readings(response));
	} catch (const std::exception &error) {
		log_api_failure("/api/v1/meter/readings", error);
		return error_response(
			webengine::http::status::service_unavailable,
			error.what());
	}
}

/**
 * @brief GET /api/v1/meter/configuration/frequency (Viewer)
 *
 * Reports the frequency measurement configuration the acquisition daemon
 * is currently running with.
 *
 * @return 200 with the configuration, or 503 when the daemon is
 *         unreachable.
 */
webengine::Response
get_frequency_configuration(AppContext &app,
			    const webengine::RequestContext &)
{
	try {
		const auto response = app.acquisition.frequency_configuration();
		require_acquisition_ok(response.status);
		return json_response(webengine::http::status::ok,
			frequency_configuration(response.frequency));
	} catch (const std::exception &error) {
		log_api_failure("/api/v1/meter/configuration/frequency",
				error);
		return error_response(
			webengine::http::status::service_unavailable,
			error.what());
	}
}

/**
 * @brief PUT /api/v1/meter/configuration/frequency (Admin)
 *
 * Validates the requested configuration, saves it through the settings
 * authority (which hot-applies it to acquisition), then reads back and
 * returns the active configuration.
 *
 * @return 200 with the applied configuration, 400 for invalid JSON or
 *         out-of-range values, or 503 when apply/readback fails.
 */
webengine::Response
put_frequency_configuration(AppContext &app,
			    const webengine::RequestContext &context)
{
	const auto correlation = request_id();
	log_api_event(mnc::logging::Priority::info,
		"frequency configuration update requested",
		"frequency_update_requested",
		{{"MNC_REQUEST_ID", correlation}});
	try {
		FrequencyConfigurationDto configuration;
		if (const auto error = glz::read_json(
			    configuration, context.request.body())) {
			log_api_event(mnc::logging::Priority::warning,
				"frequency configuration JSON is invalid",
				"frequency_update_rejected",
				{{"MNC_REQUEST_ID", correlation}});
			return error_response(
				webengine::http::status::bad_request,
				"invalid frequency configuration JSON");
		}
		(void)app.settings.update_and_save(
			[&](auto &settings) {
				settings.metering.frequency =
					frequency_settings(configuration);
			});
		const auto response = app.acquisition.frequency_configuration();
		require_acquisition_ok(response.status);
		log_api_event(mnc::logging::Priority::notice,
			"frequency configuration update applied",
			"frequency_update_applied",
			{{"MNC_REQUEST_ID", correlation},
			 {"MNC_CONFIGURATION_GENERATION",
			  std::to_string(response.configuration_generation)}});
		return json_response(webengine::http::status::ok,
			frequency_configuration(response.frequency));
	} catch (const std::invalid_argument &error) {
		log_api_event(mnc::logging::Priority::warning,
			"frequency configuration rejected: " +
				std::string(error.what()),
			"frequency_update_rejected",
			{{"MNC_REQUEST_ID", correlation}});
		return error_response(webengine::http::status::bad_request,
			error.what());
	} catch (const std::exception &error) {
		log_api_event(mnc::logging::Priority::error,
			"frequency configuration failed: " +
				std::string(error.what()),
			"frequency_update_failed",
			{{"MNC_REQUEST_ID", correlation}});
		return error_response(
			webengine::http::status::service_unavailable,
			error.what());
	}
}

} // namespace msap1::web::api
