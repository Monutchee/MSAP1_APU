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

void signed64(msap1::MeterRecord &record, std::size_t word,
	      std::int64_t value)
{
	const auto bits = std::bit_cast<std::uint64_t>(value);
	record.words[word] = static_cast<std::uint32_t>(bits);
	record.words[word + 1] = static_cast<std::uint32_t>(bits >> 32);
}

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
	for (std::size_t channel = 0; channel != 7; ++channel) {
		const auto base = 16u + channel * 5u;
		signed64(record, base, static_cast<std::int64_t>(channel));
		record.words[base + 2] = 6400;
		signed64(record, base + 3,
			 channel == 3 ? 0 : static_cast<std::int64_t>(channel + 1) *
					       1'000'000);
	}
	record.words[56] = 60001;
	record.words[57] = (1u << 0) | (1u << 1) | (1u << 2) |
			   (1u << 8) | (6u << 12) | (10u << 16);
	record.words[58] = 34'952'533;
	record.words[59] = 11;
	return record;
}

/* Format-v2 variant: cycle-defined block with an actual sample count that
 * differs from the configured window, plus the timing word and the 64-bit
 * first-sample index in words 60/61. */
msap1::MeterRecord periodic_record_v2()
{
	auto record = periodic_record();
	record.words[1] = msap1::meter_periodic_format_v2;
	record.words[6] = 6421;
	record.words[15] = 60u | (12u << 8) | (1u << 16) | (1u << 18);
	record.words[60] = 0x00000010u;
	record.words[61] = 0x00000001u;
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
		"MTR1 v1 did not decode as a basic fundamental update");
	require(!update.timing.has_value(),
		"a v1 record fabricated cycle-timing metadata");
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

void decode_v2_block_timing()
{
	const auto timestamp = std::chrono::system_clock::time_point{123s};
	auto registry = msap1::MeterDecoderRegistry::with_builtin_decoders();
	const auto update = registry.decode(periodic_record_v2(), timestamp);
	require(update.period == msap1::MeasurementPeriod::Basic &&
		update.kind == msap1::RecordKind::fundamental &&
		update.sequence == 42 && update.fundamental.has_value(),
		"MTR1 v2 did not decode as a basic fundamental update");
	require(update.timing.has_value(),
		"a v2 record decoded without cycle-timing metadata");
	const auto &timing = *update.timing;
	require(timing.sequence == 42 &&
		timing.configuration_generation == 0x12345678,
		"v2 timing identity does not match the record header");
	require(timing.first_sample_index == 0x100000010ull,
		"64-bit first-sample index was not assembled from words 60/61");
	require(timing.sample_count == 6421,
		"v2 word 6 was not decoded as the actual block sample count");
	require(timing.cycle_count == 12 &&
		timing.nominal_frequency == msap1::NominalFrequency::Hz60,
		"cycle count or nominal frequency was not decoded");
	require(timing.cycle_locked && !timing.free_run_fallback &&
		timing.first_block_after_apply,
		"cycle-lock provenance flags were not decoded");
	/* UTC state is stamped by the ingestor, never by the decoder. */
	require(timing.time_quality == msap1::TimeQuality::Unsynchronized &&
		!timing.utc_start.has_value(),
		"decoder fabricated UTC state that only the timebase knows");
	/* The actual block duration follows the actual sample count. */
	require(update.fundamental->frequency.calculation_window.sample_count ==
		6421,
		"v2 calculation window did not use the actual sample count");
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

	msap1::MeterDecoderRegistry registry;
	constexpr std::uint32_t future_format = 0x00020001;
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
		decode_v2_block_timing();
		subscriptions_and_registry_extension();
		std::cout << "meter data tests passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "meter data test failed: " << error.what() << '\n';
		return 1;
	}
}
