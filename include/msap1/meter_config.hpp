#ifndef MSAP1_METER_CONFIG_HPP
#define MSAP1_METER_CONFIG_HPP

#include "msap1/rpu_control_protocol.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace msap1 {

inline constexpr const char *default_meter_config_path =
	"/etc/monutchee/msap1/meter-conversion.json";

struct VoltageChannelConfig {
	std::uint32_t channel = 0;
	std::string name;
	double rin_ohms = 0.0;
	double rf_ohms = 0.0;
};

struct MeterConversionFile {
	std::uint32_t schema_version = 1;
	std::uint32_t rms_window_ms = 200;
	double adc_reference_volts = 2.5;
	double adc_pga_gain = 1.0;
	std::vector<VoltageChannelConfig> voltage_channels;
};

struct PreparedMeterConfiguration {
	MeterConversionFile source;
	msap1_meter_config_payload wire{};
};

PreparedMeterConfiguration load_meter_configuration(
	const std::filesystem::path &path = default_meter_config_path,
	std::uint32_t sample_rate_hz = 32000);

} // namespace msap1

#endif // MSAP1_METER_CONFIG_HPP
