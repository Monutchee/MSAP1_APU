#include "msap1/meter/measurement_timebase.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>

namespace {

using namespace std::chrono_literals;
using msap1::meter::MeasurementTimebase;
using msap1::meter::MonotonicTime;
using msap1::meter::TimeQuality;
using msap1::meter::TimeSyncPoint;
using msap1::meter::UtcEstimate;

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

/* Trusted sync point against generation 7 at 32 kSPS unless a scenario
 * overrides a field explicitly. */
TimeSyncPoint trusted_sync(std::uint64_t sample_counter, std::int64_t utc_ns,
			   std::uint64_t uncertainty_ns)
{
	return {.sample_counter = sample_counter,
		.utc_ns = utc_ns,
		.uncertainty_ns = uncertainty_ns,
		.sample_rate_hz = 32000,
		.configuration_generation = 7,
		.utc_synchronized = true};
}

std::int64_t utc_nanoseconds(const std::optional<UtcEstimate> &estimate)
{
	require(estimate.has_value(), "expected a UTC mapping to be available");
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
		       estimate->utc.time_since_epoch())
		.count();
}

void quality_state_machine()
{
	MeasurementTimebase timebase; // default 30 s staleness threshold

	require(timebase.quality(at(0s)) == TimeQuality::Unsynchronized,
		"timebase was not Unsynchronized at startup");
	require(!timebase.utc_for_sample(0, 7, at(0s)).has_value(),
		"unsynchronized timebase produced a UTC mapping");

	timebase.record_sync(trusted_sync(320'000,
					  1'000'000'000'000'000'000ll, 1000),
			     at(10s));
	require(timebase.quality(at(10s)) == TimeQuality::Synchronized,
		"a fresh trusted sync point did not synchronize the timebase");
	require(timebase.quality(at(40s)) == TimeQuality::Synchronized,
		"quality degraded before the staleness threshold elapsed");
	require(timebase.quality(at(41s)) == TimeQuality::Holdover,
		"a stale trusted sync point did not enter Holdover");
	/* Holdover still labels samples — stale is not absent — and the
	 * estimate still carries the (last known) uncertainty bound. */
	const auto holdover = timebase.utc_for_sample(320'000, 7, at(41s));
	require(holdover.has_value(),
		"Holdover dropped the UTC mapping entirely");
	require(holdover->uncertainty_ns == 1000,
		"Holdover estimate lost its uncertainty bound");

	timebase.record_sync(trusted_sync(1'600'000,
					  1'000'000'040'000'000'000ll, 1000),
			     at(50s));
	require(timebase.quality(at(50s)) == TimeQuality::Synchronized,
		"a new trusted sync point did not recover from Holdover");
}

void untrusted_sync_never_synchronizes()
{
	MeasurementTimebase timebase;

	/* A perfectly fresh sync captured from an undisciplined system clock
	 * is not UTC: quality must stay Unsynchronized and no label may be
	 * produced, no matter how recent the sync is. */
	auto untrusted = trusted_sync(64'000, 1'700'000'000'000'000'000ll, 500);
	untrusted.utc_synchronized = false;
	timebase.record_sync(untrusted, at(0s));
	require(timebase.quality(at(0s)) == TimeQuality::Unsynchronized,
		"an untrusted sync point claimed synchronization");
	require(!timebase.utc_for_sample(64'000, 7, at(0s)).has_value(),
		"an untrusted sync point produced a UTC mapping");

	/* The clock becoming disciplined recovers through a trusted sync. */
	timebase.record_sync(trusted_sync(96'000,
					  1'700'000'001'000'000'000ll, 500),
			     at(1s));
	require(timebase.quality(at(1s)) == TimeQuality::Synchronized,
		"a trusted sync did not recover from an untrusted one");
	require(timebase.utc_for_sample(96'000, 7, at(1s)).has_value(),
		"a trusted sync did not restore the UTC mapping");

	/* Losing clock discipline downgrades immediately on the next sync:
	 * an untrusted refresh replaces the trusted mapping. */
	untrusted.sample_counter = 128'000;
	timebase.record_sync(untrusted, at(2s));
	require(timebase.quality(at(2s)) == TimeQuality::Unsynchronized,
		"an untrusted refresh did not drop to Unsynchronized");
	require(!timebase.utc_for_sample(128'000, 7, at(2s)).has_value(),
		"an untrusted refresh kept producing UTC labels");

	/* And a fresh trusted sync synchronizes again. */
	timebase.record_sync(trusted_sync(160'000,
					  1'700'000'003'000'000'000ll, 500),
			     at(3s));
	require(timebase.quality(at(3s)) == TimeQuality::Synchronized,
		"a fresh trusted sync did not re-synchronize");
}

void linear_utc_mapping_uses_the_sync_rate()
{
	MeasurementTimebase timebase;
	const std::int64_t sync_utc = 1'700'000'000'000'000'000ll;
	timebase.record_sync(trusted_sync(64'000, sync_utc, 500), at(0s));

	/* Exactly at the sync point. */
	require(utc_nanoseconds(timebase.utc_for_sample(64'000, 7, at(0s))) ==
			sync_utc,
		"sync-point sample did not map to the sync UTC");
	/* One second of samples after the sync point. */
	require(utc_nanoseconds(timebase.utc_for_sample(96'000, 7, at(0s))) ==
			sync_utc + 1'000'000'000ll,
		"one second of samples did not map to one UTC second");
	/* Sub-second offset: 800 samples at 32 kSPS = 25 ms. */
	require(utc_nanoseconds(timebase.utc_for_sample(64'800, 7, at(0s))) ==
			sync_utc + 25'000'000ll,
		"fractional-second sample offset mapped incorrectly");
	/* Samples BEFORE the sync point extrapolate backwards. */
	require(utc_nanoseconds(timebase.utc_for_sample(32'000, 7, at(0s))) ==
			sync_utc - 1'000'000'000ll,
		"samples before the sync point mapped incorrectly");
	/* The uncertainty bound rides along with every estimate. */
	require(timebase.utc_for_sample(96'000, 7, at(0s))->uncertainty_ns ==
			500,
		"the sync uncertainty did not propagate into the estimate");

	/* The slope comes from the SYNC POINT's rate, not from any caller
	 * input: at the 16 kSPS the sync was latched under, the same 32'000
	 * samples take two seconds. */
	auto slower = trusted_sync(64'000, sync_utc, 500);
	slower.sample_rate_hz = 16000;
	timebase.record_sync(slower, at(1s));
	require(utc_nanoseconds(timebase.utc_for_sample(96'000, 7, at(1s))) ==
			sync_utc + 2'000'000'000ll,
		"extrapolation did not use the sync point's own sample rate");
}

/*
 * Numbers taken from the observed hardware fault: a board ~2.7 h into capture
 * at 128 kSPS, so the sync point sits at ~1.22e9 samples. A record whose
 * first-sample index was disturbed to 0 previously extrapolated back to the
 * start of capture — 9375 s, over two and a half hours — and was returned as a
 * confident label carrying the sync's small uncertainty bound, with a
 * Synchronized quality state. Nothing downstream could tell it apart from a
 * good timestamp. The decoder now rejects a zero index outright; this is the
 * second line of defence for any other implausible index.
 */
void absurd_backward_extrapolation_is_refused()
{
	MeasurementTimebase timebase;
	const std::int64_t sync_utc = 1'700'000'000'000'000'000ll;
	auto sync = trusted_sync(1'223'209'912ull, sync_utc, 3120);
	sync.sample_rate_hz = 128'000;
	timebase.record_sync(sync, at(0s));

	require(!timebase.utc_for_sample(0, 7, at(0s)).has_value(),
		"a zero sample index was still given a UTC label");

	const std::uint64_t limit =
		128'000ull *
		MeasurementTimebase::max_backward_extrapolation_seconds;
	/* Exactly at the bound is still labelled; one sample past it is not. */
	require(timebase.utc_for_sample(1'223'209'912ull - limit, 7, at(0s))
			.has_value(),
		"a sample exactly at the backward bound was refused");
	require(!timebase.utc_for_sample(1'223'209'912ull - limit - 1, 7,
					 at(0s))
			 .has_value(),
		"a sample one past the backward bound was labelled");
	/* A record in flight when the sync landed is a normal case and must
	 * keep its label: one 200 ms block behind is well inside the bound. */
	require(timebase.utc_for_sample(1'223'209'912ull - 25'600, 7, at(0s))
			.has_value(),
		"an in-flight record just behind the sync point was refused");
	/* Forward distance stays deliberately unbounded: during Holdover a
	 * record legitimately runs far ahead of the last sync, and quality()
	 * plus the uncertainty bound carry that reduced trust instead. */
	require(timebase.utc_for_sample(1'223'209'912ull + limit * 100, 7,
					at(0s))
			.has_value(),
		"a far-forward sample was refused; forward must stay unbounded");
	require(timebase.quality(at(0s)) == TimeQuality::Synchronized,
		"refusing a backward extrapolation changed the quality state");
}

void generation_mismatch_suspends_the_mapping()
{
	MeasurementTimebase timebase;
	const std::int64_t sync_utc = 1'700'000'000'000'000'000ll;
	timebase.record_sync(trusted_sync(64'000, sync_utc, 500), at(0s));

	require(timebase.utc_for_sample(64'000, 7, at(0s)).has_value(),
		"same-generation mapping was refused");
	/* The PL counter free-runs across configuration changes, so a block
	 * from another generation may sit across a rate change: refuse to
	 * extrapolate rather than label it with the wrong slope. */
	require(!timebase.utc_for_sample(64'000, 8, at(0s)).has_value(),
		"a generation mismatch did not suspend the UTC mapping");
	/* Quality is a property of the sync itself, not of any generation:
	 * the mapping is suspended, not the synchronization. */
	require(timebase.quality(at(0s)) == TimeQuality::Synchronized,
		"a generation mismatch changed the quality state");

	/* The fresh post-apply sync restores the mapping for the new
	 * generation (and retires the old one). */
	auto next_generation = trusted_sync(128'000, sync_utc +
					    2'000'000'000ll, 500);
	next_generation.configuration_generation = 8;
	timebase.record_sync(next_generation, at(1s));
	require(timebase.utc_for_sample(128'000, 8, at(1s)).has_value(),
		"a fresh sync did not restore the mapping after an apply");
	require(!timebase.utc_for_sample(64'000, 7, at(1s)).has_value(),
		"the retired generation kept its UTC mapping");
}

void utc_step_changes_mapping_only()
{
	MeasurementTimebase timebase;
	const std::int64_t before_step = 1'700'000'000'000'000'000ll;
	timebase.record_sync(trusted_sync(64'000, before_step, 500), at(0s));
	const auto sample_index = std::uint64_t{96'000};
	const auto original = utc_nanoseconds(
		timebase.utc_for_sample(sample_index, 7, at(1s)));

	/*
	 * An NTP step moves UTC forward 5 s while the sample counter has
	 * advanced exactly one second of samples. Only the label changes:
	 * the same sample index maps to a new UTC, and no counter moved.
	 */
	const std::int64_t after_step = before_step + 1'000'000'000ll +
		5'000'000'000ll;
	timebase.record_sync(trusted_sync(96'000, after_step, 500), at(1s));
	const auto remapped = utc_nanoseconds(
		timebase.utc_for_sample(sample_index, 7, at(1s)));
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
		untrusted_sync_never_synchronizes();
		linear_utc_mapping_uses_the_sync_rate();
		absurd_backward_extrapolation_is_refused();
		generation_mismatch_suspends_the_mapping();
		utc_step_changes_mapping_only();
		std::cout << "measurement timebase tests passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "measurement timebase test failed: "
			  << error.what() << '\n';
		return 1;
	}
}
