#include "msap1/meter/measurement_timebase.hpp"

namespace msap1::meter {

MeasurementTimebase::MeasurementTimebase(
	std::chrono::nanoseconds staleness_threshold)
	: staleness_threshold_(staleness_threshold)
{
}

void MeasurementTimebase::record_sync(const TimeSyncPoint &sync,
				      MonotonicTime now)
{
	std::scoped_lock lock(mutex_);
	latest_sync_ = sync;
	last_sync_at_ = now;
}

TimeQuality MeasurementTimebase::quality(MonotonicTime now) const
{
	std::scoped_lock lock(mutex_);
	/* A sync captured from an undisciplined system clock is a correlation
	 * with SOMETHING, but not with UTC: it never leaves Unsynchronized. */
	if (!latest_sync_ || !latest_sync_->utc_synchronized)
		return TimeQuality::Unsynchronized;
	/* Staleness is judged on the monotonic clock so a UTC step can never
	 * flap the quality state; only missing refreshes cause Holdover. */
	if (now - last_sync_at_ > staleness_threshold_)
		return TimeQuality::Holdover;
	return TimeQuality::Synchronized;
}

std::optional<UtcEstimate>
MeasurementTimebase::utc_for_sample(std::uint64_t sample_index,
				    std::uint32_t configuration_generation,
				    MonotonicTime now) const
{
	/* The mapping itself does not depend on freshness — a Holdover
	 * mapping is stale but still the best available label. @p now is
	 * accepted for API symmetry with quality(). */
	(void)now;
	std::scoped_lock lock(mutex_);
	if (!latest_sync_ || !latest_sync_->utc_synchronized ||
	    latest_sync_->sample_rate_hz == 0u)
		return std::nullopt;
	/*
	 * The PL counter is free-running across configuration changes, so a
	 * sync latched under another generation may sit on the far side of a
	 * sample-rate change; a single-rate extrapolation across it would be
	 * wrong. Refuse instead — the coordinator records a fresh sync right
	 * after a successful apply, so this window stays short.
	 */
	if (configuration_generation != latest_sync_->configuration_generation)
		return std::nullopt;
	/*
	 * Refuse an absurd BACKWARD extrapolation. Records are consumed
	 * forward, so a sample index can legitimately sit only slightly behind
	 * the newest sync point — by whatever was in flight when that sync
	 * landed. An index far behind it is not a late record, it is a
	 * corrupted one, and extrapolating to it would mint a confident UTC
	 * label hours in the past complete with the sync's small uncertainty
	 * bound.
	 *
	 * The bound is deliberately asymmetric. Forward distance is NOT
	 * limited: during Holdover, with no fresh sync arriving, records
	 * legitimately run arbitrarily far ahead of the last sync point, and
	 * quality() plus the uncertainty bound already convey that reduced
	 * trust. Only the backward direction is physically impossible.
	 */
	if (sample_index < latest_sync_->sample_counter) {
		const auto behind = latest_sync_->sample_counter - sample_index;
		const auto limit =
			static_cast<std::uint64_t>(latest_sync_->sample_rate_hz) *
			max_backward_extrapolation_seconds;
		if (behind > limit)
			return std::nullopt;
	}
	/*
	 * Linear extrapolation from the latest sync point at the rate that
	 * sync point was latched under. The signed delta is exact for any
	 * realistic distance between a record and its sync point; splitting
	 * into whole seconds plus a remainder keeps the nanosecond
	 * arithmetic inside int64 without losing precision.
	 */
	const auto delta = static_cast<std::int64_t>(
		sample_index - latest_sync_->sample_counter);
	const auto rate = static_cast<std::int64_t>(latest_sync_->sample_rate_hz);
	const std::int64_t whole_seconds = delta / rate;
	const std::int64_t remainder_samples = delta % rate;
	const std::int64_t utc_ns = latest_sync_->utc_ns +
		whole_seconds * 1'000'000'000ll +
		remainder_samples * 1'000'000'000ll / rate;
	/* During Holdover the last known uncertainty is reported unchanged;
	 * growing it with an oscillator drift model is future work. */
	return UtcEstimate{
		std::chrono::system_clock::time_point(
			std::chrono::duration_cast<
				std::chrono::system_clock::duration>(
				std::chrono::nanoseconds(utc_ns))),
		latest_sync_->uncertainty_ns};
}

} // namespace msap1::meter
