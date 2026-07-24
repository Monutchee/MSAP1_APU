#include "msap1/meter_health.hpp"

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

} // namespace

MeterHealth evaluate_meter_health(const AcquisitionResponse &response)
{
	const auto &adc = response.rpu_health;
	MeterHealth result;
	result.spi_responsive =
		health_flag(adc, MSAP1_ADC_HEALTH_SPI_RESPONSIVE);
	result.initialized = health_flag(adc, MSAP1_ADC_HEALTH_INITIALIZED) &&
		health_flag(adc, MSAP1_ADC_HEALTH_INIT_COMPLETE);
	result.configuration_match =
		health_flag(adc, MSAP1_ADC_HEALTH_CONFIG_MATCH);
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
	result.dc_offset_removal =
		meter_flag(adc, MSAP1_METER_HEALTH_REMOVE_DC);
	const auto frequency = response.latest_record.frequency();
	// Missing and out-of-range grid signals are valid unavailable readings.
	// Only an arithmetic error means the PL metering path itself is unhealthy.
	result.frequency_arithmetic_ok =
		response.has_meter_record != 0u && !frequency.arithmetic_error;
	result.acquisition_healthy = response.running != 0u &&
		response.has_meter_record != 0u && response.dma_read_errors == 0u &&
		response.invalid_records == 0u && response.sequence_gaps == 0u &&
		result.frequency_arithmetic_ok;
	result.adc_healthy = result.spi_responsive && result.initialized &&
		result.configuration_match && result.capture_active && result.fifo_ok &&
		result.headers_valid && result.meter_configured &&
		result.meter_generation_match;
	result.healthy = result.acquisition_healthy && result.adc_healthy;
	return result;
}

} // namespace msap1
