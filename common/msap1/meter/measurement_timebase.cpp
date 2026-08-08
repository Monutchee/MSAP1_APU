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
	if (!latest_sync_)
		return TimeQuality::Unsynchronized;
	/* Staleness is judged on the monotonic clock so a UTC step can never
	 * flap the quality state; only missing refreshes cause Holdover. */
	if (now - last_sync_at_ > staleness_threshold_)
		return TimeQuality::Holdover;
	return TimeQuality::Synchronized;
}

std::optional<std::chrono::system_clock::time_point>
MeasurementTimebase::utc_for_sample(std::uint64_t sample_index,
				    std::uint32_t sample_rate_hz,
				    MonotonicTime now) const
{
	/* The mapping itself does not depend on freshness — a Holdover
	 * mapping is stale but still the best available label. @p now is
	 * accepted for API symmetry with quality(). */
	(void)now;
	std::scoped_lock lock(mutex_);
	if (!latest_sync_ || sample_rate_hz == 0u)
		return std::nullopt;
	/*
	 * Linear extrapolation from the latest sync point. The signed delta
	 * is exact for any realistic distance between a record and its sync
	 * point; splitting into whole seconds plus a remainder keeps the
	 * nanosecond arithmetic inside int64 without losing precision.
	 */
	const auto delta = static_cast<std::int64_t>(
		sample_index - latest_sync_->sample_counter);
	const auto rate = static_cast<std::int64_t>(sample_rate_hz);
	const std::int64_t whole_seconds = delta / rate;
	const std::int64_t remainder_samples = delta % rate;
	const std::int64_t utc_ns = latest_sync_->utc_ns +
		whole_seconds * 1'000'000'000ll +
		remainder_samples * 1'000'000'000ll / rate;
	return std::chrono::system_clock::time_point(
		std::chrono::duration_cast<std::chrono::system_clock::duration>(
			std::chrono::nanoseconds(utc_ns)));
}

} // namespace msap1::meter
