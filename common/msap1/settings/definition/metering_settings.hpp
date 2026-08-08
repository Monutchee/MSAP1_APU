#pragma once

#include "msap1/meter/meter_config.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace msap1::settings {

struct RmsSettings {
	//Constraints declaration
	static constexpr std::uint32_t min_window_ms = 1;
	static constexpr std::uint32_t max_window_ms = 10000;

	//Setting Payload
	/* Superseded: the basic block is cycle-defined and the PL fallback
	 * window is derived from nominal_frequency_hz, no longer from
	 * window_ms. Kept in the schema for compatibility until a future
	 * schema bump removes it. */
	std::uint32_t window_ms = 0;
	bool remove_dc = false;

	//Validation logic
	void validate() const
	{
		if (window_ms < min_window_ms || window_ms > max_window_ms)
			throw std::runtime_error("RMS window must be 1..10000 ms");
	}
};

/** Channel-level limits live in prepare_meter_configuration(), which owns the
 *  conversion math these values feed. */
struct MeterConversionSettings {
	std::string profile_id;
	double adc_reference_volts = 0.0;
	std::vector<CurrentChannelConfig> current_channels;
	std::vector<VoltageChannelConfig> voltage_channels;
};

struct MeteringSettings {
	std::uint32_t sample_rate_hz = 0;
	/* Nominal grid frequency in Hz — metrology-wide configuration that
	 * selects the cycles-per-basic-block rule (50 -> 10, 60 -> 12) and
	 * the PL free-run fallback window. Never inferred from measurement. */
	std::uint32_t nominal_frequency_hz = 60;
	RmsSettings rms;
	FrequencyConfig frequency;
	MeterConversionSettings conversion;

	void validate() const
	{
		if (!supported_adc_sample_rate(sample_rate_hz))
			throw std::runtime_error(
				"unsupported persistent ADC sample rate");
		if (nominal_frequency_hz != 50u && nominal_frequency_hz != 60u)
			throw std::runtime_error(
				"nominal frequency must be 50 or 60 Hz");
		rms.validate();
	}
};

} // namespace msap1::settings
