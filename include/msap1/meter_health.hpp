#ifndef MSAP1_METER_HEALTH_HPP
#define MSAP1_METER_HEALTH_HPP

#include "msap1/acquisition_ipc.hpp"

namespace msap1 {

struct MeterHealth {
	bool healthy = false;
	bool acquisition_healthy = false;
	bool adc_healthy = false;
	bool spi_responsive = false;
	bool initialized = false;
	bool configuration_match = false;
	bool capture_active = false;
	bool fifo_ok = false;
	bool headers_valid = false;
	bool meter_configured = false;
	bool meter_generation_match = false;
	bool dc_offset_removal = false;
};

MeterHealth evaluate_meter_health(const AcquisitionResponse &response);

} // namespace msap1

#endif
