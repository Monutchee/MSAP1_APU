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

struct DemandSettings {
	/* One active profile at a time. Sliding is operationally responsive while
	 * fixed_block retains the aligned UTC ten-minute billing-style product. */
	std::string method = "sliding";
	std::uint32_t window_seconds = 60;

	void validate() const
	{
		if (method == "fixed_block") {
			if (window_seconds != 600)
				throw std::runtime_error(
					"fixed-block demand window must be 600 seconds");
			return;
		}
		if (method != "sliding")
			throw std::runtime_error(
				"demand method must be fixed_block or sliding");
		if (window_seconds != 60 && window_seconds != 300 &&
		    window_seconds != 600 && window_seconds != 900 &&
		    window_seconds != 1800)
			throw std::runtime_error(
				"sliding demand window must be 60, 300, 600, 900, or 1800 seconds");
	}
};

struct MeteringSettings {
	static constexpr double min_system_nominal_voltage_v = 1.0;
	static constexpr double max_system_nominal_voltage_v = 1'000'000.0;

	std::uint32_t sample_rate_hz = 0;
	/* Nominal grid frequency in Hz — metrology-wide configuration that
	 * selects the cycles-per-basic-block rule (50 -> 10, 60 -> 12) and
	 * the PL free-run fallback window. Never inferred from measurement. */
	std::uint32_t nominal_frequency_hz = 60;
	/* Presentation topology for the three voltage inputs. "wye" means the
	 * channel magnitudes and nominal reference are line-to-neutral; "delta"
	 * means line-to-line. This field supplies operator context only and does
	 * not cross the RPU/PL configuration ABI or alter sequence algorithms. */
	std::string measurement_topology = "wye";
	/* Declared voltage reference used for presentation: line-to-neutral for
	 * wye, line-to-line for delta. It does not rescale measurements or cross
	 * the RPU/PL configuration ABI. */
	double system_nominal_voltage_v = 120.0;
	RmsSettings rms;
	FrequencyConfig frequency;
	/* IEC 61000-4-30 Urms(1/2) event detection. A zero reference is
	 * the documented DISARMED state; the band ordering is enforced by
	 * prepare_meter_configuration(), which owns the threshold math. */
	PowerQualityConfig power_quality;
	DemandSettings demand;
	MeterConversionSettings conversion;

	void validate() const
	{
		if (!supported_adc_sample_rate(sample_rate_hz))
			throw std::runtime_error(
				"unsupported persistent ADC sample rate");
		if (nominal_frequency_hz != 50u && nominal_frequency_hz != 60u)
			throw std::runtime_error(
				"nominal frequency must be 50 or 60 Hz");
		if (measurement_topology != "wye" &&
		    measurement_topology != "delta")
			throw std::runtime_error(
				"measurement topology must be wye or delta");
		if (!(system_nominal_voltage_v >= min_system_nominal_voltage_v &&
		      system_nominal_voltage_v <= max_system_nominal_voltage_v))
			throw std::runtime_error(
				"system nominal voltage must be 1..1000000 V");
		rms.validate();
		demand.validate();
	}
};

} // namespace msap1::settings
