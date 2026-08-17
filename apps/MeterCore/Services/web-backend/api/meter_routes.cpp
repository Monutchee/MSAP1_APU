/**
 * @file meter_routes.cpp
 * @brief Metering endpoints: pipeline health, latest readings, and the
 *        frequency measurement configuration.
 */

#include "health_dto.hpp"
#include "meter_dto.hpp"
#include "response.hpp"
#include "routes.hpp"

#include "msap1/meter/meter_health.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <optional>
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

/**
 * Cycle-timing identity of the latest basic measurement block, embedded in
 * GET /api/v1/meter/readings.
 */
struct MeterTimingDto {
	std::uint64_t block_sequence;
	std::uint64_t first_sample_index;
	std::uint32_t sample_count;
	std::uint32_t cycle_count;
	std::uint32_t nominal_frequency_hz;
	bool cycle_locked;
	bool free_run_fallback;
	std::string time_quality;
	std::optional<std::int64_t> utc_start_nanoseconds;
	std::optional<std::uint64_t> utc_uncertainty_nanoseconds;
};

/** Body of GET /api/v1/meter/readings. */
struct MeterReadingsDto {
	std::uint64_t sequence;
	std::uint32_t configuration_generation;
	std::uint32_t sample_rate_hz;
	std::uint32_t block_sample_count;
	std::uint32_t status;
	std::uint32_t capture_frames;
	std::uint32_t header_errors;
	std::uint32_t fifo_overflows;
	std::uint32_t emit_drops;
	std::uint32_t result_drops;
	FrequencyReadingDto frequency;
	std::array<MeterChannelDto, 8> channels;
	/* Absent (omitted from the JSON) until timing provenance exists. */
	std::optional<MeterTimingDto> timing;
};

/* time_quality_name(), the channel name table, the per-channel unit, and the
 * micro-unit scaling are shared with the aggregate endpoint; they live in
 * meter_dto.hpp so both documents describe channels identically. */

/** Project the newest cached meter record onto the readings DTO. */
MeterReadingsDto readings(const msap1::MeterSnapshotResponse &response)
{
	if (!response.running || !response.has_snapshot)
		throw std::runtime_error("no meter result is available");
	const auto &snapshot = response.snapshot;
	const auto &diagnostics = response.diagnostics;
	MeterReadingsDto result{
		snapshot.sequence, snapshot.configuration_generation,
		diagnostics.sample_rate_hz, diagnostics.block_sample_count,
		diagnostics.status,
		diagnostics.capture_frames, diagnostics.header_errors,
		diagnostics.fifo_overflows, diagnostics.emit_drops,
		diagnostics.result_drops,
		{diagnostics.frequency.enabled, false,
		 diagnostics.frequency.reference_valid,
		 diagnostics.frequency.out_of_range,
		 diagnostics.frequency.timed_out,
		 diagnostics.frequency.arithmetic_error,
		 0.0, 0, diagnostics.frequency.period_q16_samples,
		 diagnostics.frequency.measurement_sequence,
		 diagnostics.frequency.mode,
		 diagnostics.frequency.reference_channel,
		 diagnostics.frequency.cycles_used},
		{},
		{},
	};
	/* Prefer the timing carried by the typed snapshot. The diagnostics timing
	 * is retained as a compatibility fallback and supplies the product-specific
	 * cycle-lock flags. Both are populated from the same ingestion-time
	 * provenance, never from the HTTP request clock. */
	if (snapshot.timing || diagnostics.timing) {
		const auto *compat = diagnostics.timing
			? &*diagnostics.timing : nullptr;
		const auto *timing = snapshot.timing ? &*snapshot.timing : nullptr;
		result.timing = MeterTimingDto{
			timing ? snapshot.sequence
			       : (compat ? compat->block_sequence : snapshot.sequence),
			timing && timing->first_sample_index
				? *timing->first_sample_index
				: (compat ? compat->first_sample_index : 0),
			timing && timing->sample_count
				? *timing->sample_count
				: (compat ? compat->sample_count : 0),
			timing && timing->cycle_count
				? *timing->cycle_count
				: (compat ? compat->cycle_count : 0),
			timing && timing->nominal_frequency_hz
				? *timing->nominal_frequency_hz
				: (compat ? compat->nominal_frequency_hz : 0),
			compat ? compat->cycle_locked : false,
			compat ? compat->free_run_fallback : false,
			timing ? time_quality_name(timing->quality)
			       : (compat ? time_quality_name(compat->time_quality)
					  : "unsynchronized"),
			timing ? timing->utc_start_nanoseconds : std::nullopt,
			timing ? timing->utc_uncertainty_nanoseconds : std::nullopt,
		};
	}
	for (std::size_t index = 0; index < result.channels.size(); ++index)
		result.channels[index] = {
			static_cast<std::uint32_t>(index),
			meter_channel_names[index], meter_channel_unit(index), false,
			diagnostics.channels[index].mean_micro_units,
			diagnostics.channels[index].rms_count, 0.0,
		};

	using Id = mnc::meter::MeterAttributeId;
	auto channel_index = [](Id id) -> std::optional<std::size_t> {
		switch (id) {
		case Id::IaRms: return 0;
		case Id::IbRms: return 1;
		case Id::IcRms: return 2;
		case Id::InRms: return 3;
		case Id::VcnRms: return 4;
		case Id::VbnRms: return 5;
		case Id::VanRms: return 6;
		default: return std::nullopt;
		}
	};
	for (const auto &reading : snapshot.values) {
		const bool valid = reading.quality ==
			mnc::meter::ReadingQuality::Valid;
		if (reading.attribute.id == Id::Frequency) {
			result.frequency.valid = valid;
			result.frequency.millihz = valid
				? static_cast<std::uint32_t>(reading.value) : 0u;
			result.frequency.hz = valid
				? static_cast<double>(reading.value) / 1000.0 : 0.0;
			continue;
		}
		if (const auto index = channel_index(reading.attribute.id)) {
			result.channels[*index].valid = valid;
			result.channels[*index].rms = valid
				? meter_units(reading.value) : 0.0;
		}
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
		 {response.transport_produced_blocks,
		  response.transport_consumed_blocks,
		  response.transport_overrun_blocks,
		  response.transport_callbacks,
		  msap1::transport_callback_deficit(response),
		  response.transport_ring_blocks},
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
		const auto response = app.acquisition.meter_snapshot();
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
 * @brief GET /api/v1/meter/aggregate (Viewer)
 *
 * Returns the newest 150/180-cycle aggregate: 15 consecutive eligible basic
 * blocks folded by the PL into one cycle-defined interval (150 cycles at a
 * 50 Hz nominal, 180 at 60 Hz).
 *
 * The document always reports `available`.  There is legitimately no
 * aggregate during the first ~3 s after a start, while capture is stopped,
 * or whenever basic blocks were ineligible, so that case is a 200 with
 * `{"available": false}` — not an error.
 *
 * The embedded frequency is INFORMATIVE ONLY: the standardized Class A
 * frequency product is defined over its own 10 s interval, which is not
 * implemented, so the object carries `informative` and deliberately no
 * validity flag.
 *
 * @return 200 with the aggregate document, or 503 when the daemon is
 *         unreachable, reports a failure status, or cached a malformed
 *         aggregate record.
 */
webengine::Response get_meter_aggregate(AppContext &app,
					const webengine::RequestContext &)
{
	try {
		const auto response = app.acquisition.information();
		require_acquisition_ok(response.status);
		const auto aggregate = meter_aggregate_dto(response);
		if (!aggregate)
			return json_response(webengine::http::status::ok,
				MeterAggregateUnavailableDto{});
		return json_response(webengine::http::status::ok, *aggregate);
	} catch (const std::exception &error) {
		log_api_failure("/api/v1/meter/aggregate", error);
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
