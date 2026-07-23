#ifndef MSAP1_METER_CONFIG_HPP
#define MSAP1_METER_CONFIG_HPP

#include "msap1/rpu_control_protocol.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace msap1 {

inline constexpr const char *default_meter_config_path =
	"/etc/monutchee/msap1/default/adc_config/acuvim3-sb-5a.json";

struct VoltageChannelConfig {
	std::uint32_t channel = 0;
	std::string name;
	bool enabled = true;
	std::uint32_t adc_pga_gain = 1;
	double rin_ohms = 0.0;
	double rf_ohms = 0.0;
};

struct CurrentChannelConfig {
	std::uint32_t channel = 0;
	std::string name;
	bool enabled = false;
	std::uint32_t adc_pga_gain = 1;
	std::string sensor_model;

	// internal_ct
	double primary_rated_amps = 0.0;
	double secondary_rated_amps = 0.0;
	double burden_ohms = 0.0;

	// voltage_output_current_sensor
	double rated_output_millivolts = 0.0;
	double frontend_gain = 1.0;
};

struct MeterConversionFile {
	std::uint32_t schema_version = 2;
	std::string profile_id;
	std::uint32_t rms_window_ms = 200;
	bool remove_dc = true;
	double adc_reference_volts = 1.0;
	std::vector<CurrentChannelConfig> current_channels;
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
