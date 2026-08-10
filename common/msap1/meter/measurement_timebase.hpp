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
 * no sync point yet, or the newest sync was captured while the system
 * clock itself was unsynchronized -> Unsynchronized; a fresh trusted sync
 * -> Synchronized; last trusted sync older than the staleness threshold
 * -> Holdover; a fresh trusted sync -> Synchronized again. One writer (the
 * periodic sync refresher) and concurrent readers (the record decode path)
 * share it, so all state is mutex-protected.
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
	/* Combined bound on the label error: bracket width, the PL
	 * elasticity-FIFO offset bound, and the system clock's own estimated
	 * error, folded in by the sync producer. */
	std::uint64_t uncertainty_ns = 0;
	/* ADC sample rate the counter was advancing at when latched. The
	 * extrapolation slope always comes from here, never from a caller,
	 * so a sync point can never be combined with the wrong rate. */
	std::uint32_t sample_rate_hz = 0;
	/* Configuration generation active at the latch instant. */
	std::uint32_t configuration_generation = 0;
	/* True when the system clock was disciplined (adjtimex !STA_UNSYNC)
	 * at the latch instant. An untrusted sync never leaves
	 * Unsynchronized: a free-running system clock is not UTC. */
	bool utc_synchronized = false;
};

/** A UTC label for one sample together with its error bound. */
struct UtcEstimate {
	std::chrono::system_clock::time_point utc{};
	/* Bound inherited from the sync point the label was derived from. */
	std::uint64_t uncertainty_ns = 0;
};

class MeasurementTimebase {
public:
	static constexpr std::chrono::seconds default_staleness_threshold{30};

	/**
	 * How far BEHIND the newest sync point a sample index may sit and still
	 * receive a UTC label. Records are consumed forward, so the only
	 * legitimate backward distance is whatever was in flight when a sync
	 * landed — far below this. Anything further is a corrupted index, not a
	 * late record, and must not be labelled. Forward distance is
	 * deliberately unbounded: Holdover legitimately runs arbitrarily far
	 * ahead of the last sync.
	 */
	static constexpr std::uint64_t max_backward_extrapolation_seconds = 60;

	explicit MeasurementTimebase(std::chrono::nanoseconds staleness_threshold =
					     default_staleness_threshold);

	/**
	 * Install a new sync point. A trusted sync (utc_synchronized) returns
	 * quality to Synchronized; an untrusted one keeps/returns it to
	 * Unsynchronized.
	 */
	void record_sync(const TimeSyncPoint &sync, MonotonicTime now);

	/** Quality as of @p now: see the state machine described above. */
	[[nodiscard]] TimeQuality quality(MonotonicTime now) const;

	/**
	 * UTC of one measurement-domain sample by linear extrapolation from
	 * the latest sync point, at THAT SYNC POINT'S sample rate. Returns
	 * nullopt while Unsynchronized (no sync, or an untrusted system
	 * clock) — a Holdover mapping is still returned, stale is not absent;
	 * quality() and the estimate's uncertainty convey the reduced trust.
	 *
	 * Also returns nullopt when @p configuration_generation differs from
	 * the sync point's: the PL counter free-runs across configuration
	 * changes, so extrapolating across a sample-rate change with this
	 * single-rate model would label samples wrongly. There is
	 * deliberately no piecewise rate timeline — a fresh sync after a
	 * configuration apply restores the mapping instead.
	 *
	 * UTC corrections arrive as new sync points and change only this
	 * mapping, never any sample index.
	 */
	[[nodiscard]] std::optional<UtcEstimate>
	utc_for_sample(std::uint64_t sample_index,
		       std::uint32_t configuration_generation,
		       MonotonicTime now) const;

private:
	const std::chrono::nanoseconds staleness_threshold_;
	mutable std::mutex mutex_;
	std::optional<TimeSyncPoint> latest_sync_;
	MonotonicTime last_sync_at_{};
};

} // namespace msap1::meter

#endif // MSAP1_MEASUREMENT_TIMEBASE_HPP
