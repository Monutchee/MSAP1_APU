#include "msap1/meter_config.hpp"

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

} // namespace

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

PreparedMeterConfiguration prepare_meter_configuration(
	MeterConversionFile source, std::uint32_t sample_rate_hz)
{
	PreparedMeterConfiguration result;
	result.source = std::move(source);
	if (result.source.schema_version != 2u)
		throw std::runtime_error("unsupported meter configuration schema");
	if (sample_rate_hz < 1000u || sample_rate_hz > 128000u ||
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
	const auto window = std::llround(
		static_cast<double>(sample_rate_hz) *
		static_cast<double>(result.source.rms_window_ms) / 1000.0);
	if (window <= 0 || window > std::numeric_limits<std::uint32_t>::max())
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

	result.wire.generation = configuration_fingerprint(result.wire);
	return result;
}

void save_meter_configuration(const MeterConversionFile &configuration,
			      const std::filesystem::path &path)
{
	const auto json = glz::write_json(configuration);
	if (!json)
		throw std::runtime_error("cannot serialize meter configuration");
	const auto parent = path.parent_path();
	if (!parent.empty())
		std::filesystem::create_directories(parent);
	const auto temporary = path.string() + ".tmp";
	{
		std::ofstream output(temporary, std::ios::trunc);
		if (!output || !(output << *json << '\n'))
			throw std::runtime_error("cannot write meter configuration " +
				temporary);
	}
	std::filesystem::rename(temporary, path);
}

} // namespace msap1
