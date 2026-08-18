#include "msap1/meter/meter_config.hpp"

#include "msap1/meter/meter_timing.hpp"

#include <glaze/glaze.hpp>

#include <cmath>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>

namespace msap1 {
namespace {

constexpr double adc_positive_codes = 8388608.0;
constexpr double q16_scale = 65536.0;
constexpr double q32_scale = 4294967296.0;
constexpr double square_root_two = 1.4142135623730950488;
/* A uniform distribution in +/- L has RMS L/sqrt(3), so an engineering
 * noise RMS converts to the PL's amplitude register via sqrt(3). */
constexpr double square_root_three = 1.7320508075688772935;

bool valid_pga_gain(std::uint32_t gain)
{
	return gain == 1u || gain == 2u || gain == 4u || gain == 8u;
}

std::uint32_t checked_coefficient(double coefficient,
				  const char *measurement)
{
	if (!std::isfinite(coefficient) || coefficient <= 0.0 ||
	    coefficient > std::numeric_limits<std::uint32_t>::max())
		throw std::runtime_error(std::string(measurement) +
			" conversion coefficient is out of range");
	return static_cast<std::uint32_t>(std::llround(coefficient));
}

std::uint32_t configuration_fingerprint(
	const msap1_meter_config_payload &configuration)
{
	const auto *bytes = reinterpret_cast<const unsigned char *>(&configuration);
	std::uint32_t hash = 2166136261u;
	for (std::size_t index = sizeof(configuration.generation);
	     index < sizeof(configuration); ++index) {
		hash ^= bytes[index];
		hash *= 16777619u;
	}
	return hash == 0u ? 1u : hash;
}

std::uint32_t frequency_mode(const std::string &mode)
{
	if (mode == "single_cycle")
		return MSAP1_FREQUENCY_MODE_SINGLE_CYCLE;
	if (mode == "rolling_cycles")
		return MSAP1_FREQUENCY_MODE_ROLLING_CYCLES;
	if (mode == "rolling_time")
		return MSAP1_FREQUENCY_MODE_ROLLING_TIME;
	throw std::runtime_error("unsupported frequency measurement mode");
}

std::uint32_t phase_q32(double phase_degrees)
{
	if (!std::isfinite(phase_degrees))
		throw std::runtime_error("simulator phase is not finite");
	double turns = std::fmod(phase_degrees / 360.0, 1.0);
	if (turns < 0.0)
		turns += 1.0;
	const auto phase = static_cast<std::uint64_t>(
		std::llround(turns * q32_scale));
	return static_cast<std::uint32_t>(phase & 0xffffffffu);
}

} // namespace

bool supported_adc_sample_rate(std::uint32_t sample_rate_hz)
{
	switch (sample_rate_hz) {
	case 1000u:
	case 2000u:
	case 4000u:
	case 8000u:
	case 16000u:
	case 32000u:
	case 64000u:
	case 128000u:
		return true;
	default:
		return false;
	}
}

PreparedMeterConfiguration load_meter_configuration(
	const std::filesystem::path &path, std::uint32_t sample_rate_hz)
{
	std::ifstream stream(path);
	if (!stream)
		throw std::runtime_error("cannot open meter configuration " +
			path.string());
	const std::string json((std::istreambuf_iterator<char>(stream)),
			       std::istreambuf_iterator<char>());

	MeterConversionFile source;
	if (const auto error = glz::read_json(source, json))
		throw std::runtime_error("invalid meter configuration " +
			path.string() + ": " + glz::format_error(error, json));
	return prepare_meter_configuration(std::move(source), sample_rate_hz);
}

MeterConversionFile decode_meter_configuration(std::string_view json)
{
	MeterConversionFile source;
	if (const auto error = glz::read<glz::opts{.error_on_unknown_keys = true}>(
		source, json))
		throw std::runtime_error("invalid meter configuration: " +
			glz::format_error(error, json));
	return source;
}

std::string encode_meter_configuration(const MeterConversionFile &configuration,
				       bool pretty)
{
	const auto encoded = pretty
		? glz::write<glz::opts{.prettify = true}>(configuration)
		: glz::write_json(configuration);
	if (!encoded)
		throw std::runtime_error("cannot serialize meter configuration");
	return *encoded;
}

PreparedMeterConfiguration prepare_meter_configuration(
	MeterConversionFile source, std::uint32_t sample_rate_hz)
{
	PreparedMeterConfiguration result;
	result.source = std::move(source);
	if (result.source.schema_version != 2u &&
	    result.source.schema_version != 3u)
		throw std::runtime_error("unsupported meter configuration schema");
	/*
	 * Schema-v2 profiles predate the simulator object. Give those physical
	 * profiles a conservative 1 A diagnostic signal that is representable by
	 * every supported current-profile gain. Schema-v3 profiles retain their
	 * explicit simulator amplitudes and are range checked below.
	 */
	if (result.source.schema_version == 2u) {
		for (auto &channel : result.source.simulator.channels) {
			if (channel.channel < 3u)
				channel.rms = 1.0;
		}
	}
	if (result.source.adc_source != "physical" &&
	    result.source.adc_source != "simulator")
		throw std::runtime_error("unsupported ADC source");
	if (!supported_adc_sample_rate(sample_rate_hz) ||
	    result.source.profile_id.empty() ||
	    result.source.rms_window_ms == 0u ||
	    result.source.rms_window_ms > 10000u ||
	    !std::isfinite(result.source.adc_reference_volts) ||
	    result.source.adc_reference_volts <= 0.0 ||
	    result.source.current_channels.empty() ||
	    result.source.voltage_channels.empty())
		throw std::runtime_error("meter configuration values are out of range");

	for (auto &gain : result.wire.adc_pga_gain)
		gain = 1u;
	result.wire.sample_rate_hz = sample_rate_hz;
	/*
	 * The basic measurement block is cycle-defined; the PL closes blocks
	 * on complete grid cycles and uses rms_window_samples only as the
	 * free-run FALLBACK window. Derive it from the nominal frequency
	 * (one nominal block: 10 cycles @ 50 Hz, 12 @ 60 Hz) — the legacy
	 * rms_window_ms no longer feeds this derivation (superseded, kept in
	 * the schema for compatibility). The division is exact at every
	 * supported sample rate for both nominals.
	 */
	if (result.source.nominal_frequency_hz != 50u &&
	    result.source.nominal_frequency_hz != 60u)
		throw std::runtime_error("nominal frequency must be 50 or 60 Hz");
	result.wire.nominal_frequency_hz = result.source.nominal_frequency_hz;
	const auto cycles = meter::cycles_per_basic_block(
		result.source.nominal_frequency_hz == 50u
			? meter::NominalFrequency::Hz50
			: meter::NominalFrequency::Hz60);
	const std::uint64_t window_numerator =
		static_cast<std::uint64_t>(sample_rate_hz) * cycles;
	if (window_numerator % result.source.nominal_frequency_hz != 0u)
		throw std::runtime_error(
			"fallback window is not an integer sample count");
	const std::uint64_t window =
		window_numerator / result.source.nominal_frequency_hz;
	if (window == 0u || window > std::numeric_limits<std::uint32_t>::max())
		throw std::runtime_error("RMS window sample count is out of range");
	result.wire.rms_window_samples = static_cast<std::uint32_t>(window);
	result.wire.flags = MSAP1_METER_CONFIG_ENABLE;
	if (result.source.remove_dc)
		result.wire.flags |= MSAP1_METER_CONFIG_REMOVE_DC;

	std::uint32_t configured_mask = 0u;
	for (const auto &channel : result.source.current_channels) {
		if (channel.channel >= 4u ||
		    (configured_mask & (1u << channel.channel)) != 0u ||
		    channel.name.empty() || !valid_pga_gain(channel.adc_pga_gain) ||
		    !std::isfinite(channel.frontend_gain) ||
		    channel.frontend_gain <= 0.0)
			throw std::runtime_error(
				"current channel configuration is invalid");
		configured_mask |= 1u << channel.channel;
		result.wire.adc_pga_gain[channel.channel] =
			static_cast<std::uint8_t>(channel.adc_pga_gain);

		double coefficient = 0.0;
		if (channel.sensor_model == "internal_ct") {
			if (!std::isfinite(channel.primary_rated_amps) ||
			    !std::isfinite(channel.secondary_rated_amps) ||
			    !std::isfinite(channel.burden_ohms) ||
			    channel.primary_rated_amps <= 0.0 ||
			    channel.secondary_rated_amps <= 0.0 ||
			    channel.burden_ohms <= 0.0)
				throw std::runtime_error(
					"internal CT configuration is invalid");
			const double ct_ratio = channel.primary_rated_amps /
				channel.secondary_rated_amps;
			coefficient = result.source.adc_reference_volts * ct_ratio *
				1000000.0 * q16_scale /
				(adc_positive_codes * channel.adc_pga_gain *
				 channel.burden_ohms);
		} else if (channel.sensor_model ==
			   "voltage_output_current_sensor") {
			if (!std::isfinite(channel.rated_output_millivolts) ||
			    channel.rated_output_millivolts <= 0.0 ||
			    (channel.enabled &&
			     (!std::isfinite(channel.primary_rated_amps) ||
			      channel.primary_rated_amps <= 0.0)))
				throw std::runtime_error(
					"voltage-output current sensor configuration is invalid");
			if (channel.enabled) {
				const double rated_output_volts =
					channel.rated_output_millivolts / 1000.0;
				coefficient = channel.primary_rated_amps * 1000000.0 *
					result.source.adc_reference_volts * q16_scale /
					(rated_output_volts * channel.frontend_gain *
					 adc_positive_codes * channel.adc_pga_gain);
			}
		} else {
			throw std::runtime_error("unsupported current sensor model");
		}

		if (!channel.enabled)
			continue;
		result.wire.scale_micro_units_q16[channel.channel] =
			checked_coefficient(coefficient, "current");
		result.wire.valid_mask |= 1u << channel.channel;
	}

	for (const auto &channel : result.source.voltage_channels) {
		if (channel.channel < 4u || channel.channel > 6u ||
		    (configured_mask & (1u << channel.channel)) != 0u ||
		    channel.name.empty() || !valid_pga_gain(channel.adc_pga_gain) ||
		    !std::isfinite(channel.rin_ohms) ||
		    !std::isfinite(channel.rf_ohms) || channel.rin_ohms <= 0.0 ||
		    channel.rf_ohms <= 0.0)
			throw std::runtime_error(
				"voltage channel configuration is invalid");
		configured_mask |= 1u << channel.channel;
		result.wire.adc_pga_gain[channel.channel] =
			static_cast<std::uint8_t>(channel.adc_pga_gain);
		if (!channel.enabled)
			continue;

		const double frontend_gain = channel.rf_ohms / channel.rin_ohms;
		const double coefficient =
			result.source.adc_reference_volts * 1000000.0 * q16_scale /
			(adc_positive_codes * channel.adc_pga_gain * frontend_gain);
		result.wire.scale_micro_units_q16[channel.channel] =
			checked_coefficient(coefficient, "voltage");
		result.wire.valid_mask |= 1u << channel.channel;
	}
	if (configured_mask != 0x7fu)
		throw std::runtime_error(
			"complete meter configuration must define channels 0 through 6");
	if (result.wire.valid_mask == 0u)
		throw std::runtime_error("meter configuration enables no channels");

	const auto &frequency = result.source.frequency;
	if (frequency.reference_channel != 6u ||
	    frequency.averaging_cycles == 0u ||
	    frequency.averaging_cycles > 64u ||
	    frequency.averaging_window_ms < 100u ||
	    frequency.averaging_window_ms > 1000u ||
	    !std::isfinite(frequency.minimum_hz) ||
	    !std::isfinite(frequency.maximum_hz) ||
	    frequency.minimum_hz < 10.0 ||
	    frequency.maximum_hz > 200.0 ||
	    frequency.minimum_hz >= frequency.maximum_hz ||
	    !std::isfinite(frequency.hysteresis_volts) ||
	    frequency.hysteresis_volts <= 0.0 ||
	    frequency.hysteresis_volts > 100.0)
		throw std::runtime_error("frequency configuration is invalid");

	result.wire.frequency_flags =
		frequency.enabled
			? static_cast<std::uint32_t>(
				MSAP1_FREQUENCY_CONFIG_ENABLE)
			: 0u;
	result.wire.frequency_mode = frequency_mode(frequency.mode);
	result.wire.frequency_reference_channel = frequency.reference_channel;
	result.wire.frequency_averaging_cycles = frequency.averaging_cycles;
	result.wire.frequency_window_samples = static_cast<std::uint32_t>(
		std::llround(static_cast<double>(sample_rate_hz) *
			     frequency.averaging_window_ms / 1000.0));
	result.wire.frequency_minimum_millihz = static_cast<std::uint32_t>(
		std::llround(frequency.minimum_hz * 1000.0));
	result.wire.frequency_maximum_millihz = static_cast<std::uint32_t>(
		std::llround(frequency.maximum_hz * 1000.0));
	result.wire.frequency_hysteresis_microvolts =
		static_cast<std::uint32_t>(
			std::llround(frequency.hysteresis_volts * 1000000.0));

	/*
	 * Convert engineering RMS values back through the same per-channel
	 * coefficient used by the PL conversion stage. The generated raw samples
	 * therefore exercise conversion, RMS, frequency, meter, and waveform logic.
	 */
	const auto &simulator = result.source.simulator;
	if (!std::isfinite(simulator.frequency_hz) ||
	    simulator.frequency_hz <= 0.0 ||
	    simulator.frequency_hz >= static_cast<double>(sample_rate_hz) / 2.0)
		throw std::runtime_error("simulator frequency is out of range");
	if (simulator.channels.size() != 7u)
		throw std::runtime_error(
			"simulator must define channels 0 through 6 exactly once");

	std::uint32_t simulator_mask = 0u;
	for (const auto &channel : simulator.channels) {
		if (channel.channel > 6u ||
		    (simulator_mask & (1u << channel.channel)) != 0u ||
		    !std::isfinite(channel.rms) || channel.rms < 0.0 ||
		    !std::isfinite(channel.dc) ||
		    !std::isfinite(channel.noise_rms) || channel.noise_rms < 0.0)
			throw std::runtime_error(
				"simulator channel configuration is invalid");
		const auto coefficient =
			result.wire.scale_micro_units_q16[channel.channel];
		/*
		 * Some physical profiles intentionally disable current channels (for
		 * example the voltage-output sensor profile until its current rating is
		 * supplied).  Such a channel cannot be expressed in engineering units,
		 * so model it as a disabled zero-valued simulator input instead of making
		 * an otherwise valid physical profile impossible to load.
		 */
		if (coefficient == 0u) {
			result.wire.simulator_peak_counts[channel.channel] = 0;
			result.wire.simulator_phase_q32[channel.channel] =
				phase_q32(channel.phase_degrees);
			result.wire.simulator_dc_offset_counts[channel.channel] = 0;
			result.wire.simulator_noise_level_counts[channel.channel] = 0;
			simulator_mask |= 1u << channel.channel;
			continue;
		}
		const double counts_per_unit = 1000000.0 * q16_scale /
			static_cast<double>(coefficient);
		const double raw_peak =
			channel.rms * square_root_two * counts_per_unit;
		if (!std::isfinite(raw_peak) || raw_peak < 0.0 ||
		    raw_peak > 8388607.0)
			throw std::runtime_error("simulator CH" +
				std::to_string(channel.channel) +
				" amplitude exceeds signed 24-bit range");
		/* DC is an instantaneous level: no sqrt(2) peak conversion. */
		const double raw_dc = channel.dc * counts_per_unit;
		if (!std::isfinite(raw_dc) || raw_dc < -8388608.0 ||
		    raw_dc > 8388607.0)
			throw std::runtime_error("simulator CH" +
				std::to_string(channel.channel) +
				" DC offset exceeds signed 24-bit range");
		/* Uniform noise amplitude register = RMS * sqrt(3). */
		const double raw_noise =
			channel.noise_rms * square_root_three * counts_per_unit;
		if (!std::isfinite(raw_noise) || raw_noise < 0.0 ||
		    raw_noise > 8388607.0)
			throw std::runtime_error("simulator CH" +
				std::to_string(channel.channel) +
				" noise amplitude exceeds signed 24-bit range");
		result.wire.simulator_peak_counts[channel.channel] =
			static_cast<std::int32_t>(std::llround(raw_peak));
		result.wire.simulator_phase_q32[channel.channel] =
			phase_q32(channel.phase_degrees);
		result.wire.simulator_dc_offset_counts[channel.channel] =
			static_cast<std::int32_t>(std::llround(raw_dc));
		result.wire.simulator_noise_level_counts[channel.channel] =
			static_cast<std::uint32_t>(std::llround(raw_noise));
		simulator_mask |= 1u << channel.channel;
	}
	if (simulator_mask != 0x7fu)
		throw std::runtime_error(
			"simulator must define channels 0 through 6 exactly once");
	result.wire.adc_source = result.source.adc_source == "simulator"
		? MSAP1_ADC_SOURCE_SIMULATOR
		: MSAP1_ADC_SOURCE_PHYSICAL;
	result.wire.simulator_frequency_millihz =
		static_cast<std::uint32_t>(
			std::llround(simulator.frequency_hz * 1000.0));
	result.wire.simulator_valid_mask = simulator_mask;
	result.wire.simulator_phase_step_q32 =
		static_cast<std::uint32_t>(std::llround(
			simulator.frequency_hz * q32_scale /
			static_cast<double>(sample_rate_hz)));
	if (result.wire.simulator_phase_step_q32 == 0u)
		throw std::runtime_error("simulator phase step is zero");
	result.wire.simulator_flags = simulator.preserve_phase
		? static_cast<std::uint32_t>(MSAP1_SIMULATOR_FLAG_PRESERVE_PHASE)
		: 0u;

	result.wire.generation = configuration_fingerprint(result.wire);
	return result;
}

void save_meter_configuration(const MeterConversionFile &configuration,
			      const std::filesystem::path &path)
{
	const auto json = encode_meter_configuration(configuration);
	const auto parent = path.parent_path();
	if (!parent.empty())
		std::filesystem::create_directories(parent);
	const auto temporary = path.string() + ".tmp";
	{
		std::ofstream output(temporary, std::ios::trunc);
		if (!output || !(output << json << '\n'))
			throw std::runtime_error("cannot write meter configuration " +
				temporary);
	}
	std::filesystem::rename(temporary, path);
}

} // namespace msap1
