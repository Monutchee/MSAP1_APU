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
#include <limits>

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

namespace utc_clock_detail {

inline std::uint64_t saturating_add(std::uint64_t left,
	std::uint64_t right) noexcept
{
	return right > std::numeric_limits<std::uint64_t>::max() - left
		? std::numeric_limits<std::uint64_t>::max()
		: left + right;
}

inline std::uint64_t saturating_microseconds_to_nanoseconds(
	long microseconds) noexcept
{
	if (microseconds <= 0)
		return 0;
	const auto value = static_cast<std::uint64_t>(microseconds);
	return value > std::numeric_limits<std::uint64_t>::max() / 1000u
		? std::numeric_limits<std::uint64_t>::max()
		: value * 1000u;
}

inline std::uint64_t magnitude(long value) noexcept
{
	if (value >= 0)
		return static_cast<std::uint64_t>(value);
	/* Avoid negating LONG_MIN. */
	return static_cast<std::uint64_t>(-(value + 1)) + 1u;
}

inline std::uint64_t offset_nanoseconds(const timex &clock_state) noexcept
{
	const auto value = magnitude(clock_state.offset);
	if ((clock_state.status & STA_NANO) != 0)
		return value;
	return value > std::numeric_limits<std::uint64_t>::max() / 1000u
		? std::numeric_limits<std::uint64_t>::max()
		: value * 1000u;
}

} // namespace utc_clock_detail

/** Convert one adjtimex snapshot into the metering clock qualification. */
inline UtcClockStatus utc_clock_status_from_timex(
	const timex &clock_state) noexcept
{
	UtcClockStatus status{};
	status.synchronized = (clock_state.status & STA_UNSYNC) == 0;
	status.positive_leap_pending = (clock_state.status & STA_INS) != 0;
	status.negative_leap_pending = (clock_state.status & STA_DEL) != 0;

	/*
	 * maxerror is not the current clock error. Linux grows it at the kernel's
	 * worst-case frequency tolerance (normally 500 ppm) between discipline
	 * updates. systemd-timesyncd therefore makes maxerror exceed the Class A
	 * 1 ms limit roughly two seconds after every otherwise-good NTP sample.
	 *
	 * Bound the current disciplined-clock estimate with the magnitude of the
	 * live PLL phase correction, the daemon-provided estimated error, and one
	 * clock-resolution quantum. Synchronization remains a separate hard gate,
	 * so an unreadable or free-running clock can never qualify through a small
	 * numeric estimate.
	 */
	status.estimated_uncertainty_ns = utc_clock_detail::saturating_add(
		utc_clock_detail::saturating_add(
			utc_clock_detail::offset_nanoseconds(clock_state),
			utc_clock_detail::saturating_microseconds_to_nanoseconds(
				clock_state.esterror)),
		utc_clock_detail::saturating_microseconds_to_nanoseconds(
			clock_state.precision));
	return status;
}

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
	return utc_clock_status_from_timex(clock_state);
}

} // namespace msap1::acquisition::daemon
