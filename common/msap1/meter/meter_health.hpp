#ifndef MSAP1_METER_HEALTH_HPP
#define MSAP1_METER_HEALTH_HPP

#include "msap1/acquisition/ipc/acquisition_commands.hpp"

#include <string>
#include <vector>

namespace msap1 {

inline constexpr std::uint32_t r5_task_stack_minimum_headroom_bytes = 2048u;

struct HealthReason {
	std::string code;
	std::string message;
};

struct MeterHealth {
	bool healthy = false;
	bool acquisition_healthy = false;
	bool record_stale = false;
	bool adc_healthy = false;
	bool simulator_active = false;
	bool simulator_healthy = false;
	bool physical_diagnostics_applicable = true;
	bool spi_responsive = false;
	bool initialized = false;
	bool configuration_match = false;
	bool rate_match = false;
	bool capture_active = false;
	bool fifo_ok = false;
	bool headers_valid = false;
	bool meter_configured = false;
	bool meter_generation_match = false;
	bool dc_offset_removal = false;
	bool frequency_arithmetic_ok = false;
	/* R5C1 aggregation offload is independent from R5C0 ADC health. During
	 * shadow validation it is observable but non-authoritative; only an
	 * authoritative R5C1 path participates in the global meter verdict. */
	bool aggregation_health_available = false;
	bool aggregation_authoritative = false;
	bool aggregation_transport_available = false;
	bool aggregation_transport_initialized = false;
	bool aggregation_input_healthy = false;
	bool aggregation_engine_ready = false;
	bool aggregation_output_ready = false;
	bool aggregation_output_active = false;
	bool aggregation_healthy = false;
	std::uint32_t adc_source = MSAP1_ADC_SOURCE_PHYSICAL;
	std::vector<HealthReason> adc_degraded_reasons;
	std::vector<HealthReason> aggregation_degraded_reasons;
};

/**
 * Period completions the driver folded into a neighbouring interrupt.
 *
 * The Xilinx cyclic callback fires per interrupt, not per period, so
 * produced_blocks minus callbacks counts the period completions that were
 * coalesced.  Purely diagnostic: no health verdict, transport decision, or
 * record accounting depends on it.  Clamped at zero because the driver's
 * callback counter is 32-bit and can wrap past the 64-bit produced total.
 */
std::uint64_t transport_callback_deficit(const InfoResponse &response);

std::vector<HealthReason>
evaluate_rpu_adc_health_reasons(const msap1_adc_health_payload &health);
std::vector<HealthReason> evaluate_rpu_aggregation_health_reasons(
	const msap1_aggregation_health_payload &health);
MeterHealth evaluate_meter_health(const InfoResponse &response);

} // namespace msap1

#endif
