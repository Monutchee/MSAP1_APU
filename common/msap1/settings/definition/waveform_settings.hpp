#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

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
	/* Neutral capture-time identity copied into every MNCWF v4 master. */
	std::string station_id;
	std::string station_name;
	std::string site_id;
	std::string site_name;
	std::string circuit_id;
	std::string circuit_name;
	/* Provisioned identity and calibration authority. Blank/unknown values are
	 * preserved as missing conversion-readiness fields; they are never guessed. */
	std::string device_serial;
	std::string calibration_id;
	std::string calibration_status = "unknown";

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
		for (const auto *value : {&station_id, &station_name, &site_id,
			&site_name, &circuit_id, &circuit_name, &device_serial,
			&calibration_id})
			if (value->size() > 128u)
				throw std::runtime_error(
					"waveform identity fields must not exceed 128 bytes");
		if (calibration_status != "unknown" &&
		    calibration_status != "valid" &&
		    calibration_status != "expired" &&
		    calibration_status != "invalid")
			throw std::runtime_error(
				"waveform calibration status must be unknown, valid, expired, or invalid");
		if (calibration_status != "unknown" && calibration_id.empty())
			throw std::runtime_error(
				"a known waveform calibration status requires a calibration ID");
	}
};

} // namespace msap1::settings
