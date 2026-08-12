#pragma once

/**
 * @file time.hpp
 * @brief Monotonic clock alias and age arithmetic shared by the daemon.
 */

#include "msap1/acquisition/ipc/acquisition_commands.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>

namespace msap1::acquisition::daemon {

/** Monotonic clock used for record freshness and audit scheduling. */
using Clock = std::chrono::steady_clock;

/**
 * @brief Age of @p timestamp in milliseconds, saturated to uint32.
 *
 * @return msap1::acquisition_age_unavailable when no timestamp exists yet
 *         (for example before the first meter record arrives).
 */
inline std::uint32_t age_milliseconds(
	const std::optional<Clock::time_point> &timestamp)
{
	if (!timestamp)
		return msap1::acquisition_age_unavailable;
	const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
				 Clock::now() - *timestamp)
				 .count();
	return static_cast<std::uint32_t>(std::clamp<std::int64_t>(
		age, 0, std::numeric_limits<std::uint32_t>::max()));
}

} // namespace msap1::acquisition::daemon
