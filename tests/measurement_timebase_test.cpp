#include "msap1/meter/measurement_timebase.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>

namespace {

using namespace std::chrono_literals;
using msap1::meter::MeasurementTimebase;
using msap1::meter::MonotonicTime;
using msap1::meter::TimeQuality;
using msap1::meter::TimeSyncPoint;

void require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

/* All monotonic instants are fabricated so the state machine is tested
 * deterministically, without sleeping through the staleness threshold. */
constexpr MonotonicTime at(std::chrono::seconds offset)
{
	return MonotonicTime{} + offset;
}

std::int64_t utc_nanoseconds(
	const std::optional<std::chrono::system_clock::time_point> &time)
{
	require(time.has_value(), "expected a UTC mapping to be available");
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
		       time->time_since_epoch())
		.count();
}

void quality_state_machine()
{
	MeasurementTimebase timebase; // default 30 s staleness threshold

	require(timebase.quality(at(0s)) == TimeQuality::Unsynchronized,
		"timebase was not Unsynchronized at startup");
	require(!timebase.utc_for_sample(0, 32000, at(0s)).has_value(),
		"unsynchronized timebase produced a UTC mapping");

	timebase.record_sync({320'000, 1'000'000'000'000'000'000ll, 1000},
			     at(10s));
	require(timebase.quality(at(10s)) == TimeQuality::Synchronized,
		"a fresh sync point did not synchronize the timebase");
	require(timebase.quality(at(40s)) == TimeQuality::Synchronized,
		"quality degraded before the staleness threshold elapsed");
	require(timebase.quality(at(41s)) == TimeQuality::Holdover,
		"a stale sync point did not enter Holdover");
	/* Holdover still labels samples — stale is not absent. */
	require(timebase.utc_for_sample(320'000, 32000, at(41s)).has_value(),
		"Holdover dropped the UTC mapping entirely");

	timebase.record_sync({1'600'000, 1'000'000'040'000'000'000ll, 1000},
			     at(50s));
	require(timebase.quality(at(50s)) == TimeQuality::Synchronized,
		"a new sync point did not recover from Holdover");
}

void linear_utc_mapping()
{
	MeasurementTimebase timebase;
	const std::int64_t sync_utc = 1'700'000'000'000'000'000ll;
	timebase.record_sync({64'000, sync_utc, 500}, at(0s));

	/* Exactly at the sync point. */
	require(utc_nanoseconds(timebase.utc_for_sample(64'000, 32000,
							at(0s))) == sync_utc,
		"sync-point sample did not map to the sync UTC");
	/* One second of samples after the sync point. */
	require(utc_nanoseconds(timebase.utc_for_sample(96'000, 32000,
							at(0s))) ==
			sync_utc + 1'000'000'000ll,
		"one second of samples did not map to one UTC second");
	/* Sub-second offset: 800 samples at 32 kSPS = 25 ms. */
	require(utc_nanoseconds(timebase.utc_for_sample(64'800, 32000,
							at(0s))) ==
			sync_utc + 25'000'000ll,
		"fractional-second sample offset mapped incorrectly");
	/* Samples BEFORE the sync point extrapolate backwards. */
	require(utc_nanoseconds(timebase.utc_for_sample(32'000, 32000,
							at(0s))) ==
			sync_utc - 1'000'000'000ll,
		"samples before the sync point mapped incorrectly");
}

void utc_step_changes_mapping_only()
{
	MeasurementTimebase timebase;
	const std::int64_t before_step = 1'700'000'000'000'000'000ll;
	timebase.record_sync({64'000, before_step, 500}, at(0s));
	const auto sample_index = std::uint64_t{96'000};
	const auto original =
		utc_nanoseconds(timebase.utc_for_sample(sample_index, 32000,
							at(1s)));

	/*
	 * An NTP step moves UTC forward 5 s while the sample counter has
	 * advanced exactly one second of samples. Only the label changes:
	 * the same sample index maps to a new UTC, and no counter moved.
	 */
	const std::int64_t after_step = before_step + 1'000'000'000ll +
		5'000'000'000ll;
	timebase.record_sync({96'000, after_step, 500}, at(1s));
	const auto remapped =
		utc_nanoseconds(timebase.utc_for_sample(sample_index, 32000,
							at(1s)));
	require(remapped == original + 5'000'000'000ll,
		"a UTC step did not shift the mapping by the step amount");
	require(remapped == after_step,
		"the sync-point sample did not map to the stepped UTC");
	/* The measurement domain is untouched: the same sample index is
	 * still the same sample index; only its wall-clock label moved. */
}

} // namespace

int main()
{
	try {
		quality_state_machine();
		linear_utc_mapping();
		utc_step_changes_mapping_only();
		std::cout << "measurement timebase tests passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "measurement timebase test failed: "
			  << error.what() << '\n';
		return 1;
	}
}
