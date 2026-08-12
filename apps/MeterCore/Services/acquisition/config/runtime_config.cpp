#include "config/runtime_config.hpp"

#include "msap1/settings/settings_ipc.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace msap1::acquisition::daemon {

msap1::settings::ProductSettings load_runtime_settings()
{
	msap1::settings::ipc::SettingsClient client;
	return client.active(5000);
}

std::array<msap1::WaveformChannelMetadata, msap1::waveform_persisted_channels>
waveform_metadata(const msap1::PreparedMeterConfiguration &configuration)
{
	static constexpr std::array<const char *,
				    msap1::waveform_persisted_channels>
		names{"Ia", "Ib", "Ic", "In", "Vc", "Vb", "Va"};
	std::array<msap1::WaveformChannelMetadata,
		   msap1::waveform_persisted_channels>
		result{};
	for (std::size_t channel = 0; channel < result.size(); ++channel) {
		auto &metadata = result[channel];
		metadata.source_channel = static_cast<std::uint32_t>(channel);
		metadata.kind = channel < 4u
			? msap1::WaveformChannelKind::current
			: msap1::WaveformChannelKind::voltage;
		metadata.scale_micro_units_q16 =
			configuration.wire.scale_micro_units_q16[channel];
		metadata.flags =
			(configuration.wire.valid_mask & (1u << channel)) != 0u
			? 1u
			: 0u;
		std::copy_n(names[channel],
			    std::min(std::strlen(names[channel]),
				     metadata.name.size() - 1u),
			    metadata.name.begin());
		const char *unit = channel < 4u ? "A" : "V";
		std::copy_n(unit, 1u, metadata.unit.begin());
	}
	return result;
}

msap1::FrequencyIpcConfiguration frequency_ipc(
	const msap1::FrequencyConfig &frequency)
{
	std::uint32_t mode = MSAP1_FREQUENCY_MODE_ROLLING_CYCLES;
	if (frequency.mode == "single_cycle")
		mode = MSAP1_FREQUENCY_MODE_SINGLE_CYCLE;
	else if (frequency.mode == "rolling_time")
		mode = MSAP1_FREQUENCY_MODE_ROLLING_TIME;
	return {
		frequency.enabled ? 1u : 0u,
		frequency.reference_channel,
		mode,
		frequency.averaging_cycles,
		frequency.averaging_window_ms,
		static_cast<std::uint32_t>(
			std::llround(frequency.minimum_hz * 1000.0)),
		static_cast<std::uint32_t>(
			std::llround(frequency.maximum_hz * 1000.0)),
		static_cast<std::uint32_t>(
			std::llround(frequency.hysteresis_volts * 1000000.0)),
	};
}

msap1::SimulatorIpcConfiguration simulator_ipc(
	const msap1::SimulatorConfig &simulator)
{
	msap1::SimulatorIpcConfiguration result{};
	result.frequency_millihz = static_cast<std::uint32_t>(
		std::llround(simulator.frequency_hz * 1000.0));
	for (const auto &channel : simulator.channels) {
		if (channel.channel >= result.channels.size())
			continue;
		result.channels[channel.channel].rms = channel.rms;
		result.channels[channel.channel].phase_degrees =
			channel.phase_degrees;
	}
	return result;
}

} // namespace msap1::acquisition::daemon
