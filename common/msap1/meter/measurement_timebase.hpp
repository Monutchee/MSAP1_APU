#ifndef MSAP1_MEASUREMENT_TIMEBASE_HPP
#define MSAP1_MEASUREMENT_TIMEBASE_HPP

/**
 * Two-time-domain measurement timebase.
 *
 * The metering pipeline deliberately keeps two independent time domains:
 *
 *  1. Measurement time — the PL 64-bit free-running conversion-domain sample
 *     counter. It increments once per accepted ADC frame, is never reset by
 *     configuration apply or capture restart, and is never stepped or slewed.
 *     Block boundaries, sample-range continuity, and gapless aggregation all
 *     live in this domain.
 *
 *  2. UTC — a civil wall-clock label attached to measurement time through
 *     discrete sync points (sample_counter, utc_ns) captured from the PL
 *     correlation latch. NTP/manual steps of the system clock change only
 *     this MAPPING; no counter and no stored record is ever rewritten.
 *
 * MeasurementTimebase owns the mapping and its quality state machine:
 * startup -> Unsynchronized; a sync point -> Synchronized; last sync older
 * than the staleness threshold -> Holdover; a fresh sync -> Synchronized
 * again. One writer (the periodic sync refresher) and concurrent readers
 * (the record decode path) share it, so all state is mutex-protected.
 */

#include "msap1/meter/meter_timing.hpp"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>

namespace msap1::meter {

/* Same monotonic clock the acquisition daemon uses for freshness tracking
 * (apps/acquisition/support/time.hpp). */
using MonotonicTime = std::chrono::steady_clock::time_point;

/** One correlation of the PL sample counter with CLOCK_REALTIME. */
struct TimeSyncPoint {
	/* PL conversion-domain sample counter at the latch instant. */
	std::uint64_t sample_counter = 0;
	/* CLOCK_REALTIME at the same instant, nanoseconds since the epoch. */
	std::int64_t utc_ns = 0;
	/* Total bound on the latch-to-clock error (bracket width plus the
	 * PL elasticity-FIFO offset bound). */
	std::uint64_t uncertainty_ns = 0;
};

class MeasurementTimebase {
public:
	static constexpr std::chrono::seconds default_staleness_threshold{30};

	explicit MeasurementTimebase(std::chrono::nanoseconds staleness_threshold =
					     default_staleness_threshold);

	/** Install a new sync point; quality returns to Synchronized. */
	void record_sync(const TimeSyncPoint &sync, MonotonicTime now);

	/** Quality as of @p now: see the state machine described above. */
	[[nodiscard]] TimeQuality quality(MonotonicTime now) const;

	/**
	 * UTC of one measurement-domain sample by linear extrapolation from
	 * the latest sync point at the given sample rate. Returns nullopt
	 * while Unsynchronized (a Holdover mapping is still usable — stale,
	 * not absent). UTC corrections arrive as new sync points and change
	 * only this mapping, never any sample index.
	 */
	[[nodiscard]] std::optional<std::chrono::system_clock::time_point>
	utc_for_sample(std::uint64_t sample_index, std::uint32_t sample_rate_hz,
		       MonotonicTime now) const;

private:
	const std::chrono::nanoseconds staleness_threshold_;
	mutable std::mutex mutex_;
	std::optional<TimeSyncPoint> latest_sync_;
	MonotonicTime last_sync_at_{};
};

} // namespace msap1::meter

#endif // MSAP1_MEASUREMENT_TIMEBASE_HPP
