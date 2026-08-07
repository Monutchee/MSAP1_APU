#ifndef MSAP1_METER_HEALTH_HPP
#define MSAP1_METER_HEALTH_HPP

#include "msap1/acquisition/ipc/acquisition_commands.hpp"

#include <string>
#include <vector>

namespace msap1 {

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
	std::uint32_t adc_source = MSAP1_ADC_SOURCE_PHYSICAL;
	std::vector<HealthReason> adc_degraded_reasons;
};

std::vector<HealthReason>
evaluate_rpu_adc_health_reasons(const msap1_adc_health_payload &health);
MeterHealth evaluate_meter_health(const InfoResponse &response);

} // namespace msap1

#endif
