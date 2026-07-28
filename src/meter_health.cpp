#include "msap1/meter_health.hpp"

#include <string>
#include <utility>

namespace msap1 {
namespace {

bool health_flag(const msap1_adc_health_payload &health, std::uint32_t flag)
{
	return (health.health_flags & flag) != 0u;
}

bool meter_flag(const msap1_adc_health_payload &health, std::uint32_t flag)
{
	return (health.meter_health_flags & flag) != 0u;
}

void append_reason(std::vector<HealthReason> &reasons, std::string code,
		   std::string message)
{
	reasons.push_back({std::move(code), std::move(message)});
}

} // namespace

std::vector<HealthReason>
evaluate_rpu_adc_health_reasons(const msap1_adc_health_payload &health)
{
	std::vector<HealthReason> reasons;
	const bool spi_snapshot_valid =
		health_flag(health, MSAP1_ADC_HEALTH_SPI_RESPONSIVE) &&
		health.spi_error == MSAP1_ADC_SPI_HEALTH_OK;
	if (!spi_snapshot_valid)
		append_reason(reasons, "spi_unresponsive",
			      "ADC SPI register-health check failed (error " +
				      std::to_string(health.spi_error) + ")");
	if (!health_flag(health, MSAP1_ADC_HEALTH_INITIALIZED))
		append_reason(reasons, "not_initialized",
			      "ADC driver is not initialized");
	/*
	 * INIT_COMPLETE and configuration-match are derived from the same SPI
	 * audit snapshot. When that snapshot is invalid, reporting them as
	 * independent failures obscures the actual transport fault.
	 */
	if (spi_snapshot_valid) {
		if (!health_flag(health, MSAP1_ADC_HEALTH_INIT_COMPLETE))
			append_reason(reasons, "init_incomplete",
				      "ADC INIT_COMPLETE is not asserted");
		if (!health_flag(health, MSAP1_ADC_HEALTH_CONFIG_MATCH))
			append_reason(
				reasons, "configuration_mismatch",
				"ADC register readback does not match the active configuration");
	}
	if (!health_flag(health, MSAP1_ADC_HEALTH_CAPTURE_ACTIVE))
		append_reason(reasons, "capture_inactive",
			      "ADC capture is not active");
	if (!health_flag(health, MSAP1_ADC_HEALTH_NO_OVERFLOW))
		append_reason(reasons, "fifo_overflow",
			      "PL capture FIFO has " +
				      std::to_string(health.overflow_count) +
				      " overflow(s)");
	if (!health_flag(health, MSAP1_ADC_HEALTH_HEADERS_VALID))
		append_reason(reasons, "invalid_headers",
			      "no valid ADC frames are available or " +
				      std::to_string(health.header_error_count) +
				      " frame header error(s) were detected");
	if (!health_flag(health, MSAP1_ADC_HEALTH_RATE_MATCH))
		append_reason(reasons, "sample_rate_mismatch",
			      "measured ADC DRDY rate " +
				      std::to_string(health.drdy_frequency_hz) +
				      " frame/s does not match configured rate " +
				      std::to_string(health.sample_rate_hz) +
				      " frame/s");
	return reasons;
}

MeterHealth evaluate_meter_health(const AcquisitionResponse &response)
{
	const auto &adc = response.rpu_health;
	MeterHealth result;
	result.adc_degraded_reasons = evaluate_rpu_adc_health_reasons(adc);
	result.spi_responsive =
		health_flag(adc, MSAP1_ADC_HEALTH_SPI_RESPONSIVE);
	result.initialized = health_flag(adc, MSAP1_ADC_HEALTH_INITIALIZED) &&
		health_flag(adc, MSAP1_ADC_HEALTH_INIT_COMPLETE);
	result.configuration_match =
		health_flag(adc, MSAP1_ADC_HEALTH_CONFIG_MATCH);
	result.rate_match = health_flag(adc, MSAP1_ADC_HEALTH_RATE_MATCH);
	result.capture_active =
		health_flag(adc, MSAP1_ADC_HEALTH_CAPTURE_ACTIVE);
	result.fifo_ok = health_flag(adc, MSAP1_ADC_HEALTH_NO_OVERFLOW);
	result.headers_valid =
		health_flag(adc, MSAP1_ADC_HEALTH_HEADERS_VALID);
	result.meter_configured =
		meter_flag(adc, MSAP1_METER_HEALTH_CORES_PRESENT) &&
		meter_flag(adc, MSAP1_METER_HEALTH_CONFIGURED) &&
		meter_flag(adc, MSAP1_METER_HEALTH_ENABLED);
	result.meter_generation_match =
		meter_flag(adc, MSAP1_METER_HEALTH_GENERATION_MATCH) &&
		adc.meter_generation == response.configuration_generation;
	if (!meter_flag(adc, MSAP1_METER_HEALTH_CORES_PRESENT))
		append_reason(result.adc_degraded_reasons,
			      "meter_cores_unavailable",
			      "PL meter-processing cores are unavailable");
	if (!meter_flag(adc, MSAP1_METER_HEALTH_CONFIGURED))
		append_reason(result.adc_degraded_reasons,
			      "meter_not_configured",
			      "PL meter configuration has not been applied");
	if (!meter_flag(adc, MSAP1_METER_HEALTH_ENABLED))
		append_reason(result.adc_degraded_reasons,
			      "meter_disabled",
			      "PL meter processing is disabled");
	if (!result.meter_generation_match)
		append_reason(result.adc_degraded_reasons,
			      "configuration_generation_mismatch",
			      "APU and PL configuration generations do not match");
	result.dc_offset_removal =
		meter_flag(adc, MSAP1_METER_HEALTH_REMOVE_DC);
	const auto frequency = response.latest_record.frequency();
	// Missing and out-of-range grid signals are valid unavailable readings.
	// Only an arithmetic error means the PL metering path itself is unhealthy.
	result.frequency_arithmetic_ok =
		response.has_meter_record != 0u && !frequency.arithmetic_error;
	result.record_stale = response.running != 0u &&
		response.meter_record_age_ms > meter_record_stale_after_ms;
	result.acquisition_healthy = response.running != 0u &&
		response.has_meter_record != 0u && response.dma_read_errors == 0u &&
		response.invalid_records == 0u && response.sequence_gaps == 0u &&
		!result.record_stale && result.frequency_arithmetic_ok;
	result.adc_healthy = result.spi_responsive && result.initialized &&
		result.configuration_match && result.rate_match &&
		result.capture_active && result.fifo_ok && result.headers_valid &&
		result.meter_configured &&
		result.meter_generation_match;
	result.healthy = result.acquisition_healthy && result.adc_healthy;
	return result;
}

} // namespace msap1
