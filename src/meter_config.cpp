#include "msap1/meter_config.hpp"

#include <glaze/glaze.hpp>

#include <cmath>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace msap1 {
namespace {

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

	PreparedMeterConfiguration result;
	if (const auto error = glz::read_json(result.source, json))
		throw std::runtime_error("invalid meter configuration " +
			path.string() + ": " + glz::format_error(error, json));
	if (result.source.schema_version != 1u)
		throw std::runtime_error("unsupported meter configuration schema");
	if (sample_rate_hz < 1000u || sample_rate_hz > 128000u ||
	    result.source.rms_window_ms == 0u ||
	    result.source.rms_window_ms > 10000u ||
	    !std::isfinite(result.source.adc_reference_volts) ||
	    result.source.adc_reference_volts <= 0.0 ||
	    !std::isfinite(result.source.adc_pga_gain) ||
	    result.source.adc_pga_gain <= 0.0 ||
	    result.source.voltage_channels.empty())
		throw std::runtime_error("meter configuration values are out of range");

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

	for (const auto &channel : result.source.voltage_channels) {
		if (channel.channel >= 8u ||
		    (result.wire.valid_mask & (1u << channel.channel)) != 0u ||
		    channel.name.empty() || !std::isfinite(channel.rin_ohms) ||
		    !std::isfinite(channel.rf_ohms) || channel.rin_ohms <= 0.0 ||
		    channel.rf_ohms <= 0.0)
			throw std::runtime_error(
				"voltage channel configuration is invalid");

		const double frontend_gain = channel.rf_ohms / channel.rin_ohms;
		const double coefficient =
			result.source.adc_reference_volts * 1000000.0 * 65536.0 /
			(8388608.0 * result.source.adc_pga_gain * frontend_gain);
		if (!std::isfinite(coefficient) || coefficient <= 0.0 ||
		    coefficient > std::numeric_limits<std::uint32_t>::max())
			throw std::runtime_error(
				"voltage conversion coefficient is out of range");
		result.wire.scale_micro_units_q16[channel.channel] =
			static_cast<std::uint32_t>(std::llround(coefficient));
		result.wire.valid_mask |= 1u << channel.channel;
	}

	result.wire.generation = configuration_fingerprint(result.wire);
	return result;
}

} // namespace msap1
