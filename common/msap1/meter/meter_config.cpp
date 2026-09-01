#include "msap1/meter/meter_config.hpp"

#include "msap1/meter/meter_timing.hpp"

#include <glaze/glaze.hpp>

#include <array>
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

std::array<const CurrentWiringChannelConfig *, 4> current_wiring_channels(
	const CurrentWiringConfig &configuration)
{
	return {&configuration.channels.ch0, &configuration.channels.ch1,
		&configuration.channels.ch2, &configuration.channels.ch3};
}

std::uint32_t current_phase_code(std::string_view phase)
{
	if (phase == "A") return MSAP1_CURRENT_PHASE_A;
	if (phase == "B") return MSAP1_CURRENT_PHASE_B;
	if (phase == "C") return MSAP1_CURRENT_PHASE_C;
	if (phase == "N") return MSAP1_CURRENT_PHASE_N;
	throw std::runtime_error("current channel phase must be A, B, C, or N");
}

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

template<class T>
void fingerprint_append(std::uint32_t &hash, const T &configuration)
{
	const auto *bytes =
		reinterpret_cast<const unsigned char *>(&configuration);
	for (std::size_t index = sizeof(configuration.generation);
	     index < sizeof(configuration); ++index) {
		hash ^= bytes[index];
		hash *= 16777619u;
	}
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

std::uint32_t harmonic_channel_mask(const std::string &channels)
{
	if (channels == "voltage")
		return 0x70u;
	if (channels == "current")
		return 0x0fu;
	if (channels == "all")
		return 0x7fu;
	throw std::runtime_error(
		"simulator harmonic channels must be voltage, current, or all");
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

void validate_current_wiring(const CurrentWiringConfig &configuration)
{
	if (configuration.input_order != "ABC" &&
	    configuration.input_order != "ACB" &&
	    configuration.input_order != "CUSTOM")
		throw std::runtime_error(
			"current wiring input_order must be ABC, ACB, or CUSTOM");

	std::array<bool, 4> seen{};
	std::uint32_t map = 0u;
	const auto channels = current_wiring_channels(configuration);
	for (std::size_t channel = 0; channel < channels.size(); ++channel) {
		const auto code = current_phase_code(channels[channel]->phase);
		if (seen[code])
			throw std::runtime_error(
				"current wiring must assign A, B, C, and N exactly once");
		seen[code] = true;
		map |= code << (channel * 2u);
		if (channels[channel]->direction != "normal" &&
		    channels[channel]->direction != "reversed")
			throw std::runtime_error(
				"current channel direction must be normal or reversed");
	}
	if (configuration.input_order == "ABC" && map != 0xe4u)
		throw std::runtime_error(
			"ABC current wiring preset does not match its channel assignments");
	if (configuration.input_order == "ACB" && map != 0xd8u)
		throw std::runtime_error(
			"ACB current wiring preset does not match its channel assignments");
}

std::uint32_t current_adc_phase_map(
	const CurrentWiringConfig &configuration)
{
	validate_current_wiring(configuration);
	std::uint32_t result = 0u;
	const auto channels = current_wiring_channels(configuration);
	for (std::size_t channel = 0; channel < channels.size(); ++channel)
		result |= current_phase_code(channels[channel]->phase) <<
			(channel * 2u);
	return result;
}

std::uint32_t current_adc_invert_mask(
	const CurrentWiringConfig &configuration)
{
	validate_current_wiring(configuration);
	std::uint32_t result = 0u;
	const auto channels = current_wiring_channels(configuration);
	for (std::size_t channel = 0; channel < channels.size(); ++channel)
		if (channels[channel]->direction == "reversed")
			result |= 1u << channel;
	return result;
}

std::uint32_t physical_current_channel_for_logical(
	const CurrentWiringConfig &configuration, std::uint32_t logical_channel)
{
	if (logical_channel >= 4u)
		throw std::runtime_error("logical current channel is out of range");
	validate_current_wiring(configuration);
	const auto channels = current_wiring_channels(configuration);
	for (std::size_t channel = 0; channel < channels.size(); ++channel)
		if (current_phase_code(channels[channel]->phase) == logical_channel)
			return static_cast<std::uint32_t>(channel);
	throw std::runtime_error("logical current channel is not assigned");
}

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

void coordinate_configuration_generation(
	msap1_meter_config_payload &meter,
	msap1_m18_config_payload &m18) noexcept
{
	std::uint32_t hash = 2166136261u;
	fingerprint_append(hash, meter);
	fingerprint_append(hash, m18);
	if (hash == 0u)
		hash = 1u;
	meter.generation = hash;
	m18.generation = hash;
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
	    result.source.schema_version != 3u &&
	    result.source.schema_version != 4u &&
	    result.source.schema_version != 5u)
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
	if (result.source.schema_version < 5u)
		result.source.current_wiring = CurrentWiringConfig{};
	result.source.schema_version = 5u;
	validate_current_wiring(result.source.current_wiring);
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
	result.wire.current_adc_phase_map =
		current_adc_phase_map(result.source.current_wiring);
	result.wire.current_adc_invert_mask =
		current_adc_invert_mask(result.source.current_wiring);

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
		const auto logical_channel =
			(result.wire.current_adc_phase_map >> (channel.channel * 2u)) &
			0x3u;
		result.wire.valid_mask |= 1u << logical_channel;
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
	 * Urms(1/2) event detection. A zero reference is the documented
	 * DISARMED state and is left unchecked against the band; anything
	 * else must describe a usable band, because thresholds that cross
	 * would declare events that mean nothing.
	 */
	const auto &pq = result.source.power_quality;
	const auto pq_fraction = [](double percent) {
		return static_cast<std::uint32_t>(std::llround(percent * 100.0));
	};
	if (!std::isfinite(pq.reference_volts) || pq.reference_volts < 0.0 ||
	    pq.reference_volts > 1000000.0 ||
	    !std::isfinite(pq.sag_percent) || !std::isfinite(pq.swell_percent) ||
	    !std::isfinite(pq.interruption_percent) ||
	    !std::isfinite(pq.hysteresis_percent) ||
	    pq.interruption_percent < 0.0 || pq.swell_percent > 655.0 ||
	    pq.hysteresis_percent < 0.0)
		throw std::runtime_error(
			"power-quality configuration values are out of range");
	if (pq.reference_volts > 0.0 &&
	    (pq.interruption_percent >= pq.sag_percent ||
	     pq.sag_percent >= pq.swell_percent ||
	     pq.hysteresis_percent >= pq.sag_percent))
		throw std::runtime_error(
			"power-quality thresholds must be ordered "
			"interruption < sag < swell with a smaller hysteresis");
	result.wire.pq_reference_microvolts = static_cast<std::uint32_t>(
		std::llround(pq.reference_volts * 1000000.0));
	result.wire.pq_sag_threshold_e4 = pq_fraction(pq.sag_percent);
	result.wire.pq_swell_threshold_e4 = pq_fraction(pq.swell_percent);
	result.wire.pq_interruption_threshold_e4 =
		pq_fraction(pq.interruption_percent);
	result.wire.pq_hysteresis_e4 = pq_fraction(pq.hysteresis_percent);

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
	if (simulator.harmonics.size() > 4u)
		throw std::runtime_error(
			"simulator supports at most 4 harmonic slots");
	for (std::size_t slot = 0; slot < simulator.harmonics.size(); ++slot) {
		const auto &harmonic = simulator.harmonics[slot];
		if (!std::isfinite(harmonic.order) || harmonic.order <= 1.0 ||
		    harmonic.order >= 128.0)
			throw std::runtime_error(
				"simulator harmonic ratio must be greater than 1 and less than 128");
		if (!std::isfinite(harmonic.percent) ||
		    harmonic.percent < 0.0 || harmonic.percent > 99.9)
			throw std::runtime_error(
				"simulator harmonic percent must be 0 through 99.9");
		const auto ratio_q16 = static_cast<std::uint32_t>(
			std::llround(harmonic.order * q16_scale));
		if (ratio_q16 <= 0x00010000u || ratio_q16 >= 0x00800000u)
			throw std::runtime_error(
				"simulator harmonic ratio is not representable in range");
		const std::uint64_t tone_millihz =
			(static_cast<std::uint64_t>(ratio_q16) *
			 result.wire.simulator_frequency_millihz) >> 16;
		if (tone_millihz * 2u >=
		    static_cast<std::uint64_t>(sample_rate_hz) * 1000u)
			throw std::runtime_error(
				"simulator harmonic exceeds the Nyquist limit");
		const auto fraction = static_cast<std::uint32_t>(
			std::llround(harmonic.percent / 100.0 * q16_scale));
		result.wire.simulator_harmonics[slot * 3] = ratio_q16;
		result.wire.simulator_harmonics[slot * 3 + 1] =
			harmonic_channel_mask(harmonic.channels) | (fraction << 16);
		result.wire.simulator_harmonics[slot * 3 + 2] =
			phase_q32(harmonic.phase_degrees);
	}

	const auto &am = simulator.amplitude_modulation;
	if (!std::isfinite(am.frequency_hz) || !std::isfinite(am.depth_percent) ||
	    am.frequency_hz <= 0.0 || am.frequency_hz >= 1000.0 ||
	    am.depth_percent < 0.0 || am.depth_percent > 100.0)
		throw std::runtime_error(
			"simulator amplitude-modulation configuration is out of range");
	if (am.enabled && am.depth_percent == 0.0)
		throw std::runtime_error(
			"enabled simulator amplitude modulation requires non-zero depth");
	if (am.enabled &&
	    am.frequency_hz >= static_cast<double>(sample_rate_hz) / 2.0)
		throw std::runtime_error(
			"simulator amplitude modulation exceeds the Nyquist limit");
	if (am.enabled) {
		result.wire.simulator_am_frequency_millihz =
			static_cast<std::uint32_t>(std::llround(am.frequency_hz * 1000.0));
		result.wire.simulator_am_depth_q16 = static_cast<std::uint32_t>(
			std::llround(am.depth_percent / 100.0 * q16_scale));
		result.wire.simulator_am_channel_mask =
			harmonic_channel_mask(am.channels);
	}

	const auto &carrier = simulator.carrier;
	const auto valid_tone = [sample_rate_hz](double frequency, double percent,
		std::string_view description) {
		if (!std::isfinite(frequency) || !std::isfinite(percent) ||
		    frequency <= 0.0 ||
		    frequency >= static_cast<double>(sample_rate_hz) / 2.0 ||
		    percent < 0.0 || percent > 99.9)
			throw std::runtime_error("simulator " + std::string(description) +
				" tone is out of range");
	};
	if (carrier.phase_mask == 0u || (carrier.phase_mask & ~0x7u) != 0u)
		throw std::runtime_error(
			"simulator carrier phase mask must select voltage phases A/B/C");
	if (carrier.enabled && carrier.percent == 0.0)
		throw std::runtime_error(
			"enabled simulator carrier requires non-zero amplitude");
	if (carrier.enabled) {
		valid_tone(carrier.frequency_hz, carrier.percent, "carrier");
		if (carrier.adjacent_percent != 0.0)
			valid_tone(carrier.adjacent_frequency_hz,
				carrier.adjacent_percent, "adjacent");
		result.wire.simulator_carrier_frequency_millihz =
			static_cast<std::uint32_t>(
				std::llround(carrier.frequency_hz * 1000.0));
		result.wire.simulator_carrier_fraction_q16 =
			static_cast<std::uint32_t>(
				std::llround(carrier.percent / 100.0 * q16_scale));
		result.wire.simulator_carrier_phase_mask =
			(carrier.phase_mask & 0x7u) << 4;
		result.wire.simulator_carrier_phase_q32 =
			phase_q32(carrier.phase_degrees);
		if (carrier.adjacent_percent != 0.0) {
			result.wire.simulator_adjacent_frequency_millihz =
				static_cast<std::uint32_t>(std::llround(
					carrier.adjacent_frequency_hz * 1000.0));
			result.wire.simulator_adjacent_fraction_q16 =
				static_cast<std::uint32_t>(std::llround(
					carrier.adjacent_percent / 100.0 * q16_scale));
			result.wire.simulator_adjacent_phase_q32 =
				phase_q32(carrier.adjacent_phase_degrees);
		}
	}

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
