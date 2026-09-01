#pragma once

/**
 * @file health_dto.hpp
 * @brief Health transfer objects and acquisition-status helpers shared by
 *        several route modules.
 *
 * The meter health projection is rendered verbatim by
 * GET /api/v1/meter/health and embedded (plus web-stack state) in
 * GET /api/v1/health, so its DTOs live here instead of inside one
 * *_routes.cpp.  The small acquisition helpers below are shared for the
 * same reason: every module that talks to the acquisition daemon validates
 * reply status, and both the health and ADC modules name ADC sources.
 */

#include "msap1/acquisition/ipc/acquisition_ipc.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace msap1::web::api {

/** One reason code explaining why ADC health is degraded. */
struct HealthReasonDto {
	std::string code;
	std::string message;
};

/**
 * Kernel DMA transport accounting, as reported by the driver.
 *
 * callback_deficit is produced_blocks minus callbacks: the Xilinx cyclic
 * callback fires per interrupt, not per period, so it counts the period
 * completions that were coalesced.  Diagnostic only — no health verdict
 * depends on it.
 */
struct TransportHealthDto {
	std::uint64_t produced_blocks;
	std::uint64_t consumed_blocks;
	std::uint64_t overrun_blocks;
	std::uint64_t callbacks;
	std::uint64_t callback_deficit;
	std::uint32_t ring_blocks;
};

/** Acquisition daemon transport and meter-record freshness health. */
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
	/** Rejections since the current capture epoch began. */
	std::uint64_t invalid_records;
	/** Process-lifetime forensic total. */
	std::uint64_t lifetime_invalid_records;
	std::uint64_t sequence_gaps;
	TransportHealthDto dma_transport;
	std::uint32_t configuration_generation;
};

struct CurrentWiringChannelDto {
	std::string phase;
	std::string direction;
};

struct CurrentWiringChannelsDto {
	CurrentWiringChannelDto ch0;
	CurrentWiringChannelDto ch1;
	CurrentWiringChannelDto ch2;
	CurrentWiringChannelDto ch3;
};

struct CurrentWiringConfigurationDto {
	std::string input_order;
	CurrentWiringChannelsDto channels;
	std::uint32_t phase_map;
	std::uint32_t invert_mask;
};

struct CurrentWiringHealthDto {
	CurrentWiringConfigurationDto requested;
	CurrentWiringConfigurationDto active;
	std::uint32_t generation;
	bool match;
	std::string last_apply_result;
	std::uint32_t readback_mismatch_count;
};

/** ADC front-end health as audited by the RPU and measured in PL. */
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
	CurrentWiringHealthDto current_wiring;
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
	std::string source;
	bool physical_diagnostics_available;
	bool simulator_healthy;
	std::uint32_t simulator_active_generation;
	std::uint32_t simulator_frame_count;
	std::uint32_t simulator_saturation_count;
	std::uint32_t simulator_missed_sample_count;
	std::vector<HealthReasonDto> degraded_reasons;
};

/** Cached R5C1 aggregation-offload health, independent from R5C0 ADC health. */
struct AggregationHealthDto {
	bool available;
	bool healthy;
	bool authoritative;
	bool transport_available;
	bool transport_initialized;
	bool input_healthy;
	bool engine_ready;
	bool output_ready;
	bool output_active;
	bool probe_pending;
	std::uint32_t probe_failures;
	std::uint32_t cache_age_ms;
	std::string rpmsg_device;
	std::uint32_t health_flags;
	std::uint32_t frames_received;
	std::uint32_t frames_valid;
	std::uint32_t frames_invalid;
	std::uint32_t crc_errors;
	std::uint32_t format_errors;
	std::uint32_t sequence_gaps;
	std::uint32_t ring_overflows;
	std::uint32_t software_ring_push_failures;
	std::uint32_t input_records_dropped;
	std::uint32_t first_dropped_sequence;
	std::uint32_t last_dropped_sequence;
	std::uint32_t fifo_errors;
	std::uint32_t length_errors;
	std::uint32_t records_queued;
	std::uint32_t records_emitted;
	std::uint32_t output_errors;
	std::uint32_t output_drops;
	std::uint32_t basic_completed;
	std::uint32_t aggregate_completed;
	std::uint32_t ten_minute_completed;
	std::uint32_t two_hour_completed;
	std::uint32_t software_ring_current;
	std::uint32_t software_ring_high_water;
	std::uint32_t software_ring_capacity;
	std::uint32_t software_ring_pressure;
	std::uint32_t software_ring_warning_entries;
	std::uint32_t software_ring_high_entries;
	std::uint32_t software_ring_critical_entries;
	std::uint32_t software_ring_full_entries;
	std::uint32_t hardware_fifo_current_words;
	std::uint32_t hardware_fifo_high_water_words;
	std::uint32_t hardware_fifo_full_events;
	std::uint32_t input_wake_count;
	std::uint32_t input_records_processed;
	std::uint32_t input_max_batch;
	std::uint32_t input_max_runtime_us;
	std::uint32_t validator_wake_count;
	std::uint32_t validator_records_processed;
	std::uint32_t validator_max_runtime_us;
	std::uint32_t validator_max_schedule_gap_us;
	std::uint32_t control_stack_high_water_bytes;
	std::uint32_t input_stack_high_water_bytes;
	std::uint32_t output_stack_high_water_bytes;
	std::uint32_t validator_stack_high_water_bytes;
	std::vector<HealthReasonDto> degraded_reasons;
};

/** Metering pipeline health, the body of GET /api/v1/meter/health. */
struct MeterHealthDto {
	bool healthy;
	AcquisitionHealthDto acquisition;
	AdcHealthDto adc;
	AggregationHealthDto aggregation;
	bool frequency_arithmetic_ok;
};

/**
 * @brief Throw when an acquisition IPC reply carries a non-ok status.
 *
 * @param status The status field of any acquisition daemon response.
 * @throws std::runtime_error naming the numeric status on failure.
 */
inline void require_acquisition_ok(msap1::AcquisitionStatus status)
{
	if (status != msap1::AcquisitionStatus::ok)
		throw std::runtime_error(
			"acquisition daemon returned status " +
			std::to_string(static_cast<std::uint32_t>(status)));
}

/**
 * @brief Map an MSAP1_ADC_SOURCE_* wire value to its JSON name.
 *
 * @param source Wire value from an acquisition response.
 * @return "physical", "simulator", or "unknown".
 */
inline std::string adc_source_name(std::uint32_t source)
{
	if (source == MSAP1_ADC_SOURCE_PHYSICAL)
		return "physical";
	if (source == MSAP1_ADC_SOURCE_SIMULATOR)
		return "simulator";
	return "unknown";
}

/**
 * @brief Project an acquisition InfoResponse onto the meter health DTO.
 *
 * Evaluates the product health policy (msap1::evaluate_meter_health) and
 * flattens the RPU ADC audit into JSON-friendly fields.  Defined in
 * meter_routes.cpp.
 *
 * @pre response.rpu_health has a value (the daemon always includes it in an
 *      ok InfoResponse).
 */
[[nodiscard]] MeterHealthDto
meter_health_dto(const msap1::InfoResponse &response);

} // namespace msap1::web::api
