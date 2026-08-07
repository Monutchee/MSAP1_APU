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
	std::uint64_t invalid_records;
	std::uint64_t sequence_gaps;
	std::uint32_t configuration_generation;
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

/** Metering pipeline health, the body of GET /api/v1/meter/health. */
struct MeterHealthDto {
	bool healthy;
	AcquisitionHealthDto acquisition;
	AdcHealthDto adc;
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
