#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace msap1 {

struct SocTemperatureReading {
	std::string zone;
	std::string label;
	std::optional<std::int64_t> millidegrees_c;

	bool available() const noexcept { return millidegrees_c.has_value(); }
	double celsius() const noexcept
	{
		return millidegrees_c
			? static_cast<double>(*millidegrees_c) / 1000.0
			: 0.0;
	}
};

/*
 * Discover the ZynqMP temperature sensors by their kernel hwmon labels.
 * hwmon indices are assigned dynamically, so callers must not assume that
 * the device remains /sys/class/hwmon/hwmon1 across boots or kernel updates.
 */
std::array<SocTemperatureReading, 3> read_soc_temperatures(
	const std::filesystem::path &hwmon_root = "/sys/class/hwmon");

} // namespace msap1
