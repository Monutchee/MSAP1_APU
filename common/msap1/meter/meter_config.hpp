#ifndef MSAP1_METER_CONFIG_HPP
#define MSAP1_METER_CONFIG_HPP

#include "msap1/acquisition/rpu/rpu_control_protocol.h"

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
	/* Constant offset in engineering units (volts/amps); default flat. */
	double dc = 0.0;
	/* RMS of the uniform white fluctuation the PL adds, engineering
	 * units. Real grid inputs never sit bit-flat; a small value here
	 * makes simulated readings jitter realistically. 0 disables. */
	double noise_rms = 0.0;
};

struct SimulatorHarmonicConfig {
	/* Harmonic/interharmonic frequency ratio, >1 and <128. Integer values
	 * are harmonic orders; fractional values inject interharmonics. */
	double order = 0.0;
	/* Amplitude as a percentage of each receiving lane's fundamental
	 * peak (0..99.9; the PL fraction register is Q16 < 1.0). */
	double percent = 0.0;
	/* Extra phase in degrees ON TOP of the physical rule (the PL
	 * scales each lane's fundamental offset by the ratio, so a 3rd
	 * harmonic on a balanced set lands zero-sequence by itself). */
	double phase_degrees = 0.0;
	/* Which lanes receive it: "voltage", "current", or "all". */
	std::string channels = "voltage";
};

struct SimulatorAmplitudeModulationConfig {
	bool enabled = false;
	double frequency_hz = 8.8;
	double depth_percent = 0.0;
	/* "voltage", "current", or "all". */
	std::string channels = "voltage";
};

struct SimulatorCarrierConfig {
	bool enabled = false;
	double frequency_hz = 1000.0;
	double percent = 0.0;
	/* Conceptual voltage phases A/B/C in bits 0..2. */
	std::uint32_t phase_mask = 0x7u;
	double phase_degrees = 0.0;
	/* A zero adjacent percent disables this independent rejection tone. */
	double adjacent_frequency_hz = 1020.0;
	double adjacent_percent = 0.0;
	double adjacent_phase_degrees = 0.0;
};

/*
 * IEC 61000-4-30 Urms(1/2) event detection (metrology M12). Everything
 * here is in engineering units like the rest of this file; the PL wants
 * micro-volts and 1e-4 fractions and gets them at prepare time.
 */
struct PowerQualityConfig {
	/* Declared reference voltage Udin, VOLTS. ZERO DISABLES DETECTION:
	 * the PL keeps publishing Urms(1/2) snapshots but never declares an
	 * event, so a meter whose reference has not been set cannot invent
	 * dips. Set it to the nominal line-neutral voltage. */
	double reference_volts = 0.0;
	/* Thresholds as a PERCENT of the reference. The band must be
	 * ordered interruption < sag < swell, and the hysteresis (added on
	 * recovery from a sag, subtracted on recovery from a swell) must be
	 * smaller than the sag threshold. */
	double sag_percent = 90.0;
	double swell_percent = 110.0;
	double interruption_percent = 10.0;
	double hysteresis_percent = 2.0;
};

struct SimulatorConfig {
	double frequency_hz = 60.0;
	/* Keep the generated waveform's phase/framing across a
	 * reconfiguration commit instead of restarting at 0 degrees. */
	bool preserve_phase = false;
	std::vector<SimulatorChannelConfig> channels{
		{0u, 5.0, 0.0, 0.0, 0.0},
		{1u, 5.0, -120.0, 0.0, 0.0},
		{2u, 5.0, 120.0, 0.0, 0.0},
		{3u, 0.0, 0.0, 0.0, 0.0},
		{4u, 120.0, 120.0, 0.0, 0.0},
		{5u, 120.0, -120.0, 0.0, 0.0},
		{6u, 120.0, 0.0, 0.0, 0.0},
	};
	/* Up to four global harmonic slots; empty keeps a pure tone. */
	std::vector<SimulatorHarmonicConfig> harmonics{};
	SimulatorAmplitudeModulationConfig amplitude_modulation{};
	SimulatorCarrierConfig carrier{};
};

struct MeterConversionFile {
	std::uint32_t schema_version = 4;
	std::string profile_id;
	std::string adc_source = "physical";
	/* Superseded: kept for schema compatibility; the PL window is now
	 * derived from nominal_frequency_hz (see below), not window_ms. */
	std::uint32_t rms_window_ms = 200;
	/* Nominal grid frequency (50 or 60). Defaults to 60 so schema-v3
	 * conversion files written before this field existed keep loading
	 * without a schema bump (default-when-absent). */
	std::uint32_t nominal_frequency_hz = 60;
	bool remove_dc = true;
	double adc_reference_volts = 1.0;
	std::vector<CurrentChannelConfig> current_channels;
	std::vector<VoltageChannelConfig> voltage_channels;
	// Kept optional-by-default for compatible schema-v2 profiles created
	// before frequency measurement was introduced.
	FrequencyConfig frequency;
	SimulatorConfig simulator;
	/* Optional-by-default like `frequency`: profiles written before
	 * event detection existed keep loading with detection disarmed. */
	PowerQualityConfig power_quality;
};

struct PreparedMeterConfiguration {
	MeterConversionFile source;
	msap1_meter_config_payload wire{};
	msap1_m18_config_payload m18_wire{};
};

bool supported_adc_sample_rate(std::uint32_t sample_rate_hz);
PreparedMeterConfiguration load_meter_configuration(
	const std::filesystem::path &path,
	std::uint32_t sample_rate_hz = 128000);
PreparedMeterConfiguration prepare_meter_configuration(
	MeterConversionFile source, std::uint32_t sample_rate_hz = 128000);
void coordinate_configuration_generation(
	msap1_meter_config_payload &meter,
	msap1_m18_config_payload &m18) noexcept;
[[nodiscard]] MeterConversionFile decode_meter_configuration(
	std::string_view json);
[[nodiscard]] std::string encode_meter_configuration(
	const MeterConversionFile &configuration, bool pretty = false);
void save_meter_configuration(const MeterConversionFile &configuration,
			      const std::filesystem::path &path);

} // namespace msap1

#endif // MSAP1_METER_CONFIG_HPP
