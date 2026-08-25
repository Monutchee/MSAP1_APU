#include "msap1/meter/meter_health.hpp"

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

bool aggregation_flag(const msap1_aggregation_health_payload &health,
		      std::uint32_t flag)
{
	return (health.health_flags & flag) != 0u;
}

void append_reason(std::vector<HealthReason> &reasons, std::string code,
		   std::string message)
{
	reasons.push_back({std::move(code), std::move(message)});
}

} // namespace

std::uint64_t transport_callback_deficit(const InfoResponse &response)
{
	if (response.transport_callbacks >= response.transport_produced_blocks)
		return 0;
	return response.transport_produced_blocks - response.transport_callbacks;
}

std::vector<HealthReason>
evaluate_rpu_adc_health_reasons(const msap1_adc_health_payload &health)
{
	std::vector<HealthReason> reasons;
	const bool simulator = health.adc_source == MSAP1_ADC_SOURCE_SIMULATOR;
	if (simulator) {
		if (!health_flag(health, MSAP1_ADC_HEALTH_INITIALIZED))
			append_reason(reasons, "not_initialized",
				      "ADC simulator is not initialized");
		if (!health_flag(health, MSAP1_ADC_HEALTH_SIMULATOR_HEALTHY))
			append_reason(reasons, "simulator_unhealthy",
				      "PL ADC simulator configuration or status is invalid");
	} else if (health.adc_source != MSAP1_ADC_SOURCE_PHYSICAL) {
		append_reason(reasons, "invalid_adc_source",
			      "RPU reports an unsupported ADC source");
	}
	const bool spi_snapshot_valid =
		health_flag(health, MSAP1_ADC_HEALTH_SPI_RESPONSIVE) &&
		health.spi_error == MSAP1_ADC_SPI_HEALTH_OK;
	if (!simulator && !spi_snapshot_valid)
		append_reason(reasons, "spi_unresponsive",
			      "ADC SPI register-health check failed (error " +
				      std::to_string(health.spi_error) + ")");
	if (!simulator && !health_flag(health, MSAP1_ADC_HEALTH_INITIALIZED))
		append_reason(reasons, "not_initialized",
			      "ADC driver is not initialized");
	/*
	 * INIT_COMPLETE and configuration-match are derived from the same SPI
	 * audit snapshot. When that snapshot is invalid, reporting them as
	 * independent failures obscures the actual transport fault.
	 */
	if (!simulator && spi_snapshot_valid) {
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

std::vector<HealthReason> evaluate_rpu_aggregation_health_reasons(
	const msap1_aggregation_health_payload &health)
{
	std::vector<HealthReason> reasons;
	if (!aggregation_flag(
		    health, MSAP1_AGGREGATION_HEALTH_TRANSPORT_AVAILABLE))
		append_reason(reasons, "transport_unavailable",
			      "R5C1 AXI FIFO transport is unavailable");
	if (!aggregation_flag(
		    health, MSAP1_AGGREGATION_HEALTH_TRANSPORT_INITIALIZED))
		append_reason(reasons, "transport_not_initialized",
			      "R5C1 AXI FIFO transport is not initialized");
	if (!aggregation_flag(health, MSAP1_AGGREGATION_HEALTH_INPUT_HEALTHY))
		append_reason(reasons, "input_unhealthy",
			      "R5C1 aggregation input stream is not healthy");
	if (health.fifo_errors != 0u)
		append_reason(reasons, "fifo_errors",
			      "R5C1 reports " +
				      std::to_string(health.fifo_errors) +
				      " AXI FIFO error(s)");
	if (health.crc_errors != 0u || health.format_errors != 0u ||
	    health.length_errors != 0u)
		append_reason(reasons, "invalid_input_frames",
			      "R5C1 rejected aggregation input frame(s)");
	if (health.ring_overflows != 0u)
		append_reason(reasons, "input_ring_overflow",
			      "R5C1 aggregation input ring overflowed");
	if (health.software_ring_push_failures != 0u)
		append_reason(
			reasons, "input_ring_push_failures",
			"R5C1 could not enqueue " +
				std::to_string(health.software_ring_push_failures) +
				" aggregation input record(s)");
	if (health.input_records_dropped != 0u) {
		std::string message =
			"R5C1 deterministically dropped " +
			std::to_string(health.input_records_dropped) +
			" aggregation input record(s)";
		if (health.first_dropped_sequence != 0u ||
		    health.last_dropped_sequence != 0u)
			message += " (source sequences " +
				std::to_string(health.first_dropped_sequence) +
				" through " +
				std::to_string(health.last_dropped_sequence) + ")";
		append_reason(reasons, "input_records_dropped",
			      std::move(message));
	}
	if (health.software_ring_pressure >=
	    MSAP1_AGGREGATION_RING_PRESSURE_CRITICAL)
		append_reason(
			reasons, "input_ring_pressure_critical",
			"R5C1 aggregation input ring pressure is critical (" +
				std::to_string(health.software_ring_current) + "/" +
				std::to_string(health.software_ring_capacity) +
				" records occupied)");

	/* Engine/output readiness is required only after R5C1 declares itself
	 * authoritative. During shadow validation these zero flags document the
	 * incomplete cutover rather than a production meter failure. */
	if (aggregation_flag(health, MSAP1_AGGREGATION_HEALTH_AUTHORITATIVE)) {
		if (!aggregation_flag(health,
				      MSAP1_AGGREGATION_HEALTH_ENGINE_READY))
			append_reason(reasons, "engine_not_ready",
				      "R5C1 aggregation engine is not ready");
		if (!aggregation_flag(health,
				      MSAP1_AGGREGATION_HEALTH_OUTPUT_READY))
			append_reason(reasons, "output_not_ready",
				      "R5C1 aggregation output is not ready");
		if (!aggregation_flag(health,
				      MSAP1_AGGREGATION_HEALTH_OUTPUT_ACTIVE))
			append_reason(reasons, "output_inactive",
				      "R5C1 aggregation output is inactive");
		if (health.output_errors != 0u)
			append_reason(reasons, "output_errors",
				      "R5C1 reports aggregation output errors");
		if (health.output_drops != 0u)
			append_reason(reasons, "output_drops",
				      "R5C1 dropped aggregation output records");
	}
	return reasons;
}

MeterHealth evaluate_meter_health(const InfoResponse &response)
{
	const auto adc = response.rpu_health.value();
	MeterHealth result;
	result.adc_source = adc.adc_source;
	result.simulator_active =
		adc.adc_source == MSAP1_ADC_SOURCE_SIMULATOR;
	result.simulator_healthy =
		health_flag(adc, MSAP1_ADC_HEALTH_SIMULATOR_HEALTHY);
	result.physical_diagnostics_applicable =
		health_flag(adc, MSAP1_ADC_HEALTH_PHYSICAL_DIAGNOSTICS);
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
		response.has_meter_record && !frequency.arithmetic_error;
	result.aggregation_health_available = response.has_aggregation_health;
	if (response.has_aggregation_health) {
		const auto aggregation = response.rpu_aggregation_health.value();
		result.aggregation_authoritative = aggregation_flag(
			aggregation, MSAP1_AGGREGATION_HEALTH_AUTHORITATIVE);
		result.aggregation_transport_available = aggregation_flag(
			aggregation,
			MSAP1_AGGREGATION_HEALTH_TRANSPORT_AVAILABLE);
		result.aggregation_transport_initialized = aggregation_flag(
			aggregation,
			MSAP1_AGGREGATION_HEALTH_TRANSPORT_INITIALIZED);
		result.aggregation_input_healthy = aggregation_flag(
			aggregation, MSAP1_AGGREGATION_HEALTH_INPUT_HEALTHY);
		result.aggregation_engine_ready = aggregation_flag(
			aggregation, MSAP1_AGGREGATION_HEALTH_ENGINE_READY);
		result.aggregation_output_ready = aggregation_flag(
			aggregation, MSAP1_AGGREGATION_HEALTH_OUTPUT_READY);
		result.aggregation_output_active = aggregation_flag(
			aggregation, MSAP1_AGGREGATION_HEALTH_OUTPUT_ACTIVE);
		result.aggregation_degraded_reasons =
			evaluate_rpu_aggregation_health_reasons(aggregation);
		result.aggregation_healthy =
			result.aggregation_degraded_reasons.empty();
	}
	result.record_stale = response.running &&
		response.meter_record_age_ms > meter_record_stale_after_ms;
	result.acquisition_healthy = response.running &&
		response.has_meter_record && response.dma_read_errors == 0u &&
		response.invalid_records == 0u && response.sequence_gaps == 0u &&
		!result.record_stale && result.frequency_arithmetic_ok;
	const bool source_healthy = result.simulator_active
		? result.simulator_healthy
		: result.spi_responsive && result.initialized &&
			result.configuration_match;
	result.adc_healthy = source_healthy && result.rate_match &&
		result.capture_active && result.fifo_ok && result.headers_valid &&
		result.meter_configured &&
		result.meter_generation_match;
	result.healthy = result.acquisition_healthy && result.adc_healthy &&
		(!result.aggregation_authoritative ||
		 result.aggregation_healthy);
	return result;
}

} // namespace msap1
