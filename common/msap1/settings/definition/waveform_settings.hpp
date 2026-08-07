#pragma once

#include <cstdint>
#include <stdexcept>

namespace msap1::settings {

struct WaveformSettings {
	//Constraints declaration
	static constexpr std::uint32_t max_trigger_window_ms = 120000;

	//Setting Payload
	std::uint32_t default_pretrigger_ms = 0;
	std::uint32_t default_posttrigger_ms = 0;

	//Validation logic
	void validate() const
	{
		if (default_pretrigger_ms > max_trigger_window_ms ||
		    default_posttrigger_ms > max_trigger_window_ms)
			throw std::runtime_error(
				"waveform defaults must not exceed 120 seconds");
	}
};

} // namespace msap1::settings
