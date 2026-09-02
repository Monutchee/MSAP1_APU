#pragma once

#include <stdexcept>
#include <string>

namespace msap1::settings {

/** Operator-selected system-clock discipline. NTP remains the default. */
struct TimeSettings {
	std::string synchronization = "ntp";

	void validate() const
	{
		if (synchronization != "ntp" && synchronization != "ptp")
			throw std::runtime_error(
				"time synchronization must be 'ntp' or 'ptp'");
	}
};

} // namespace msap1::settings
