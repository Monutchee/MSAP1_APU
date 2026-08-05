#ifndef MSAP1_METER_CONFIG_HPP
#define MSAP1_METER_CONFIG_HPP

#include "msap1/rpu_control_protocol.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace msap1 {

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

struct FrequencyConfig {
	bool enabled = true;
	std::uint32_t reference_channel = 6;
	std::string mode = "rolling_cycles";
	std::uint32_t averaging_cycles = 10;
	std::uint32_t averaging_window_ms = 1000;
	double minimum_hz = 40.0;
	double maximum_hz = 70.0;
	double hysteresis_volts = 1.0;
};

struct SimulatorChannelConfig {
	std::uint32_t channel = 0;
	double rms = 0.0;
	double phase_degrees = 0.0;
};

struct SimulatorConfig {
	double frequency_hz = 60.0;
	std::vector<SimulatorChannelConfig> channels{
		{0u, 5.0, 0.0},
		{1u, 5.0, -120.0},
		{2u, 5.0, 120.0},
		{3u, 0.0, 0.0},
		{4u, 120.0, 120.0},
		{5u, 120.0, -120.0},
		{6u, 120.0, 0.0},
	};
};

struct MeterConversionFile {
	std::uint32_t schema_version = 3;
	std::string profile_id;
	std::string adc_source = "physical";
	std::uint32_t rms_window_ms = 200;
	bool remove_dc = true;
	double adc_reference_volts = 1.0;
	std::vector<CurrentChannelConfig> current_channels;
	std::vector<VoltageChannelConfig> voltage_channels;
	// Kept optional-by-default for compatible schema-v2 profiles created
	// before frequency measurement was introduced.
	FrequencyConfig frequency;
	SimulatorConfig simulator;
};

struct PreparedMeterConfiguration {
	MeterConversionFile source;
	msap1_meter_config_payload wire{};
};

bool supported_adc_sample_rate(std::uint32_t sample_rate_hz);
PreparedMeterConfiguration load_meter_configuration(
	const std::filesystem::path &path,
	std::uint32_t sample_rate_hz = 32000);
PreparedMeterConfiguration prepare_meter_configuration(
	MeterConversionFile source, std::uint32_t sample_rate_hz = 32000);
[[nodiscard]] MeterConversionFile decode_meter_configuration(
	std::string_view json);
[[nodiscard]] std::string encode_meter_configuration(
	const MeterConversionFile &configuration, bool pretty = false);
void save_meter_configuration(const MeterConversionFile &configuration,
			      const std::filesystem::path &path);

} // namespace msap1

#endif // MSAP1_METER_CONFIG_HPP
