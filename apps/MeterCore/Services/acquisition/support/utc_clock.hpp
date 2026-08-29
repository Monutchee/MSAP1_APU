#pragma once

/**
 * @file utc_clock.hpp
 * @brief Linux system-clock discipline state via adjtimex(2).
 *
 * The measurement timebase must not claim a Synchronized UTC mapping when
 * the system clock itself is free-running: CLOCK_REALTIME is only UTC while
 * the kernel reports it disciplined (NTP/chrony/phc2sys). This helper stays
 * in the daemon — msap1::meter remains pure and testable, with tests
 * constructing TimeSyncPoint directly.
 */

#include <cstdint>

#include <sys/timex.h>

namespace msap1::acquisition::daemon {

/** Kernel view of the CLOCK_REALTIME discipline state. */
struct UtcClockStatus {
	/* True when the kernel clock is disciplined (!STA_UNSYNC). */
	bool synchronized = false;
	bool positive_leap_pending = false;
	bool negative_leap_pending = false;
	/* Kernel maximum-error estimate for the clock itself, nanoseconds. */
	std::uint64_t estimated_uncertainty_ns = 0;
};

/**
 * @brief Read the kernel clock discipline state (read-only adjtimex call).
 *
 * A failed query reports an unsynchronized clock: an unreadable discipline
 * state must degrade time quality, never inflate it.
 */
inline UtcClockStatus read_utc_clock_status() noexcept
{
	timex clock_state{};
	/* modes == 0: pure query, never adjusts the clock. */
	if (::adjtimex(&clock_state) < 0)
		return {};
	UtcClockStatus status{};
	status.synchronized = (clock_state.status & STA_UNSYNC) == 0;
	status.positive_leap_pending = (clock_state.status & STA_INS) != 0;
	status.negative_leap_pending = (clock_state.status & STA_DEL) != 0;
	/* maxerror is maintained by the kernel in microseconds. */
	status.estimated_uncertainty_ns =
		static_cast<std::uint64_t>(clock_state.maxerror) * 1000u;
	return status;
}

} // namespace msap1::acquisition::daemon
