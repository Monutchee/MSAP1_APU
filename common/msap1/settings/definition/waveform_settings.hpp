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
	/**
	 * Capture-file decimation divisor: every stored sample is the mean of
	 * this many acquisition frames. Power of two so groups divide evenly
	 * into the WFM1 block structure; 32 bottoms out around 67 samples per
	 * 60 Hz cycle at the 128 kSPS acquisition rate.
	 */
	std::uint32_t default_decimation = 1;

	static bool valid_decimation(std::uint32_t decimation)
	{
		return decimation == 1u || decimation == 2u || decimation == 4u ||
			decimation == 8u || decimation == 16u || decimation == 32u;
	}

	//Validation logic
	void validate() const
	{
		if (default_pretrigger_ms > max_trigger_window_ms ||
		    default_posttrigger_ms > max_trigger_window_ms)
			throw std::runtime_error(
				"waveform defaults must not exceed 120 seconds");
		if (!valid_decimation(default_decimation))
			throw std::runtime_error(
				"waveform decimation must be 1, 2, 4, 8, 16, or 32");
	}
};

} // namespace msap1::settings
