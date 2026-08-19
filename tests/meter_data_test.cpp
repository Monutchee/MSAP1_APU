#include "msap1/meter/meter_data.hpp"

#include <bit>
#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {

using namespace std::chrono_literals;

void require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

template<typename Callable>
void require_throws(Callable &&callable, const char *message)
{
	try {
		callable();
	} catch (const std::invalid_argument &) {
		return;
	}
	throw std::runtime_error(message);
}

void signed64(msap1::MeterRecord &record, std::size_t word,
	      std::int64_t value)
{
	const auto bits = std::bit_cast<std::uint64_t>(value);
	record.words[word] = static_cast<std::uint32_t>(bits);
	record.words[word + 1] = static_cast<std::uint32_t>(bits >> 32);
}

/* Minimal valid MTR1 record: a locked 12-cycle block at exactly the 60 Hz
 * nominal (6400 samples at 32 kSPS), the 64-bit first-sample index in
 * envelope words 9/10, the timing word at 13, and the transport drop words
 * 11/12 zero — as every emitted record carries them. */
msap1::MeterRecord periodic_record()
{
	msap1::MeterRecord record{};
	record.words[0] = msap1::meter_record_magic;
	record.words[1] = msap1::meter_periodic_format;
	record.words[2] = msap1::meter_record_size;
	record.words[3] = 42;
	record.words[4] = 0x12345678;
	record.words[5] = 32000;
	record.words[6] = 6400;
	record.words[7] = 0x7f;
	record.words[9] = 0x00000010u;
	record.words[10] = 0x00000001u;
	record.words[13] = 60u | (12u << 8) | (1u << 16) | (1u << 18);
	for (std::size_t channel = 0; channel != 7; ++channel) {
		const auto base = 16u + channel * 5u;
		signed64(record, base, static_cast<std::int64_t>(channel));
		record.words[base + 2] = 6400;
		signed64(record, base + 3,
			 channel == 3 ? 0 : static_cast<std::int64_t>(channel + 1) *
					       1'000'000);
	}
	/* BASIC-v4: block last-sample anchor and merged line-line RMS. */
	record.words[14] = 0x00001a0fu;
	record.words[15] = 0x00000001u;
	record.words[51] = 12'000'000;
	record.words[52] = 11'000'000;
	record.words[53] = 13'000'000;
	record.words[56] = 60001;
	record.words[57] = (1u << 0) | (1u << 1) | (1u << 2) |
			   (1u << 8) | (6u << 12) | (10u << 16);
	record.words[58] = 34'952'533;
	record.words[59] = 11;
	return record;
}

void decode_and_period_independence()
{
	const auto timestamp = std::chrono::system_clock::time_point{123s};
	auto registry = msap1::MeterDecoderRegistry::with_builtin_decoders();
	const auto update = registry.decode(periodic_record(), timestamp);
	require(update.period == msap1::MeasurementPeriod::Basic &&
		update.kind == msap1::RecordKind::fundamental &&
		update.sequence == 42 && update.fundamental.has_value(),
		"MTR1 did not decode as a basic fundamental update");
	require(update.timing.has_value(),
		"an MTR1 record decoded without cycle-timing metadata");
	const auto &values = *update.fundamental;
	require(values.frequency.valid() && values.frequency.value == 60001 &&
		values.frequency.measured_at == timestamp &&
		values.frequency.calculation_window.sample_count == 6400 &&
		values.frequency.calculation_window.duration == 200ms,
		"frequency metadata was not preserved");
	require(values.voltage_ln.phase_a.value == 7'000'000 &&
		values.voltage_ln.phase_b.value == 6'000'000 &&
		values.voltage_ln.phase_c.value == 5'000'000,
		"hardware Vc/Vb/Va order was not mapped to phase A/B/C");
	require(values.current.neutral.valid() &&
		values.current.neutral.value == 0,
		"valid zero current was confused with unavailable current");
	require(values.voltage_ll.phase_a.valid() &&
		values.voltage_ll.phase_a.value == 12'000'000 &&
		values.voltage_ll.phase_b.value == 11'000'000 &&
		values.voltage_ll.phase_c.value == 13'000'000,
		"BASIC-v4 line-line words 51..53 were not decoded");

	msap1::MeterLatestStore store;
	store.apply(update);
	auto aggregate = update;
	aggregate.period = msap1::MeasurementPeriod::Cycles150_180;
	aggregate.sequence = 100;
	aggregate.fundamental->frequency.value = 59990;
	store.apply(aggregate);
	require(store.latest(msap1::MeasurementPeriod::Basic)->values.fundamental
			.frequency.value == 60001 &&
		store.latest(msap1::MeasurementPeriod::Cycles150_180)
			->values.fundamental.frequency.value == 59990,
		"independent measurement periods inherited values from each other");
	require(!store.latest(msap1::MeasurementPeriod::Min10),
		"missing period did not remain unavailable");

	auto older = update;
	older.sequence = 41;
	older.fundamental->frequency.value = 1;
	store.apply(older);
	require(store.latest(msap1::MeasurementPeriod::Basic)->values.fundamental
			.frequency.value == 60001,
		"out-of-order update replaced newer state");
}

void decode_block_timing()
{
	const auto timestamp = std::chrono::system_clock::time_point{123s};
	auto registry = msap1::MeterDecoderRegistry::with_builtin_decoders();
	/* A locked block need not match the nominal window: the actual
	 * sample count varies with grid frequency. */
	auto record = periodic_record();
	record.words[6] = 6421;
	const auto update = registry.decode(record, timestamp);
	require(update.period == msap1::MeasurementPeriod::Basic &&
		update.kind == msap1::RecordKind::fundamental &&
		update.sequence == 42 && update.fundamental.has_value(),
		"MTR1 did not decode as a basic fundamental update");
	require(update.timing.has_value(),
		"an MTR1 record decoded without cycle-timing metadata");
	const auto &timing = *update.timing;
	require(timing.sequence == 42 &&
		timing.configuration_generation == 0x12345678,
		"the timing identity does not match the record header");
	require(timing.first_sample_index == 0x100000010ull,
		"64-bit first-sample index was not assembled from words 9/10");
	require(timing.sample_count == 6421,
		"word 6 was not decoded as the actual block sample count");
	require(timing.cycle_count == 12 &&
		timing.nominal_frequency == msap1::NominalFrequency::Hz60,
		"cycle count or nominal frequency was not decoded");
	require(timing.cycle_locked && !timing.free_run_fallback &&
		timing.first_block_after_apply,
		"cycle-lock provenance flags were not decoded");
	/* UTC state is stamped by the ingestor, never by the decoder. */
	require(timing.time_quality == msap1::TimeQuality::Unsynchronized &&
		!timing.utc_start.has_value() &&
		!timing.utc_uncertainty_ns.has_value(),
		"decoder fabricated UTC state that only the timebase knows");
	/* The actual block duration follows the actual sample count. */
	require(update.fundamental->frequency.calculation_window.sample_count ==
		6421,
		"the calculation window did not use the actual sample count");
}

/*
 * Malformed timing must never silently become a valid basic measurement
 * block: the decoder rejects shapes the PL cannot legitimately produce.
 */
void decode_rejects_malformed_timing()
{
	auto registry = msap1::MeterDecoderRegistry::with_builtin_decoders();

	/* A cycle-locked block must close exactly the nominal's cycle count
	 * (12 at 60 Hz); 11 is impossible without lost zero crossings. */
	auto wrong_cycles = periodic_record();
	wrong_cycles.words[13] = 60u | (11u << 8) | (1u << 16);
	require_throws([&] { (void)registry.decode(wrong_cycles); },
		       "a locked block with a wrong cycle count decoded");

	/* A block with zero samples has no measurement in it. */
	auto empty_block = periodic_record();
	empty_block.words[6] = 0;
	require_throws([&] { (void)registry.decode(empty_block); },
		       "a zero-sample block decoded");

	/* first_sample_index + sample_count must stay inside the 64-bit
	 * conversion counter. */
	auto overflowing = periodic_record();
	overflowing.words[9] = 0xffffffffu;
	overflowing.words[10] = 0xffffffffu;
	require_throws([&] { (void)registry.decode(overflowing); },
		       "an overflowing sample range decoded");

	/* A zero first-sample index is unreachable: the PL conversion stage
	 * issues index 1 for the first accepted frame and never resets the
	 * counter. Observed on hardware as a disturbed provenance field, and it
	 * must not decode — accepting it anchors the block's UTC label at the
	 * start of capture while still reporting a small uncertainty bound. */
	auto zero_index = periodic_record();
	zero_index.words[9] = 0;
	zero_index.words[10] = 0;
	require_throws([&] { (void)registry.decode(zero_index); },
		       "a zero first-sample index decoded");

	/* Only exact zero is impossible; index 1 is the genuine first block. */
	auto first_ever = periodic_record();
	first_ever.words[9] = 1;
	first_ever.words[10] = 0;
	const auto first_update = registry.decode(first_ever);
	require(first_update.timing.has_value() &&
		first_update.timing->first_sample_index == 1u,
		"the first block of a capture was rejected");

	/* Free-run fallback blocks are time-defined: any cycle count is
	 * legitimate there, including zero on a dead grid. */
	auto fallback = periodic_record();
	fallback.words[13] = 60u | (0u << 8) | (1u << 17);
	const auto update = registry.decode(fallback);
	require(update.timing.has_value() &&
		update.timing->free_run_fallback &&
		!update.timing->cycle_locked &&
		update.timing->cycle_count == 0,
		"a zero-cycle free-run fallback block did not decode");
}

/*
 * The retired v1 (0x00010001) and v2 (0x00010002) format words are UNKNOWN
 * now — PL and APU ship together, so a record carrying one is a stale image
 * or corruption, never a format to decode. Deliberate regression guard.
 */
void decode_rejects_retired_formats()
{
	auto registry = msap1::MeterDecoderRegistry::with_builtin_decoders();
	for (const std::uint32_t retired : {0x00010001u, 0x00010002u}) {
		auto record = periodic_record();
		record.words[1] = retired;
		require(!record.header_valid(),
			"a retired format word passed header validation");
		require_throws([&] { (void)registry.decode(record); },
			       "a retired MTR1 format word decoded");
	}
}

void class_a_aggregation_eligibility()
{
	using msap1::meter::class_a_aggregation_eligible;
	auto registry = msap1::MeterDecoderRegistry::with_builtin_decoders();

	/* Complete locked blocks aggregate: 12 cycles at 60 Hz... */
	auto locked_60 = periodic_record();
	locked_60.words[13] = 60u | (12u << 8) | (1u << 16);
	require(class_a_aggregation_eligible(
			*registry.decode(locked_60).timing),
		"a complete locked 60 Hz block was not eligible");

	/* ...and 10 cycles at 50 Hz. */
	auto locked_50 = periodic_record();
	locked_50.words[13] = 50u | (10u << 8) | (1u << 16);
	require(class_a_aggregation_eligible(
			*registry.decode(locked_50).timing),
		"a complete locked 50 Hz block was not eligible");

	/* Free-run fallback is time-defined data: never aggregated. */
	auto fallback = periodic_record();
	fallback.words[13] = 60u | (12u << 8) | (1u << 17);
	require(!class_a_aggregation_eligible(
			*registry.decode(fallback).timing),
		"a free-run fallback block was eligible");

	/* The first block after an apply is conservatively ineligible even
	 * when flagged locked (defense in depth: today's PL RTL cannot emit
	 * first_block_after_apply together with cycle_locked). */
	auto first_after_apply = periodic_record();
	first_after_apply.words[13] = 60u | (12u << 8) | (1u << 16) |
		(1u << 18);
	require(!class_a_aggregation_eligible(
			*registry.decode(first_after_apply).timing),
		"the first block after apply was eligible");

	/* A locked block with the wrong cycle count no longer survives the
	 * decoder, so the eligibility term is exercised on a hand-built
	 * BlockTiming (e.g. one replayed from a stored stream). */
	msap1::BlockTiming wrong_count{};
	wrong_count.cycle_locked = true;
	wrong_count.free_run_fallback = false;
	wrong_count.first_block_after_apply = false;
	wrong_count.cycle_count = 11;
	wrong_count.nominal_frequency = msap1::NominalFrequency::Hz60;
	require(!class_a_aggregation_eligible(wrong_count),
		"a partial locked block was eligible");
	wrong_count.cycle_count = 12;
	require(class_a_aggregation_eligible(wrong_count),
		"the hand-built complete locked block was not eligible");
}

void subscriptions_and_registry_extension()
{
	msap1::MeterData data;
	std::uint32_t notifications = 0;
	std::mutex notification_mutex;
	std::condition_variable notification_condition;
	{
		auto subscription = data.subscribe(msap1::MeasurementPeriod::Basic,
			[&](const auto &view) {
				require(view.latest_sequence == 42,
					"subscription delivered wrong sequence");
				{
					std::scoped_lock lock(notification_mutex);
					++notifications;
				}
				notification_condition.notify_one();
			});
		data.apply(msap1::decode_periodic_meter_record(periodic_record()));
		std::unique_lock lock(notification_mutex);
		require(notification_condition.wait_for(lock, 1s, [&] {
				return notifications == 1;
			}),
			"meter subscription was not delivered asynchronously");
	}
	data.apply(msap1::decode_periodic_meter_record(periodic_record()));
	std::this_thread::sleep_for(20ms);
	require(notifications == 1,
		"meter subscription was not removed by its lifetime token");

	/* A callback that is slower than acquisition must not delay apply(). The
	 * subscriber receives latest-state notifications on its own worker and may
	 * coalesce intermediate values. */
	auto slow = data.subscribe(msap1::MeasurementPeriod::Basic,
		[](const auto &) { std::this_thread::sleep_for(100ms); });
	const auto started = std::chrono::steady_clock::now();
	for (auto sequence = 43u; sequence != 53u; ++sequence) {
		auto update = msap1::decode_periodic_meter_record(periodic_record());
		update.sequence = sequence;
		data.apply(update);
	}
	require(std::chrono::steady_clock::now() - started < 50ms,
		"slow latest-state subscriber blocked meter ingestion");

	/* Record type 0x0002 is the real MTR2 aggregate, so the demo of
	 * registering a future decoder uses the next unassigned type. */
	msap1::MeterDecoderRegistry registry;
	constexpr std::uint32_t future_format = 0x00030001;
	registry.register_decoder(future_format,
		[](const msap1::MeterRecord &, msap1::SystemTime) {
			msap1::MeterUpdate update{};
			update.period = msap1::MeasurementPeriod::Min10;
			update.kind = msap1::RecordKind::demand;
			update.sequence = 77;
			update.configuration_generation = 9;
			update.demand = msap1::DemandValues{};
			return update;
		});
	auto future = periodic_record();
	future.words[1] = future_format;
	const auto decoded = registry.decode(future);
	require(decoded.period == msap1::MeasurementPeriod::Min10 &&
		decoded.kind == msap1::RecordKind::demand && decoded.demand,
		"future decoder could not be registered independently");
}

} // namespace

int main()
{
	try {
		decode_and_period_independence();
		decode_block_timing();
		decode_rejects_malformed_timing();
		decode_rejects_retired_formats();
		class_a_aggregation_eligibility();
		subscriptions_and_registry_extension();
		std::cout << "meter data tests passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "meter data test failed: " << error.what() << '\n';
		return 1;
	}
}
