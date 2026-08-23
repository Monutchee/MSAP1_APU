#include "msap1/meter/meter_data.hpp"

#include "support/reference_aggregator.hpp"

#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>

/*
 * MTR2 (0x00020002) aggregate-record tests: the test-only reference
 * aggregator against golden vectors, the decoder against records built from
 * reference output, and the defensive validation of shapes the PL must
 * never emit. The PL is the authoritative aggregator; nothing here computes
 * aggregates in production code.
 */

namespace {

using namespace std::chrono_literals;
using msap1::meter::basic_blocks_per_aggregate;
using msap1::testing::ReferenceBasicInput;
using msap1::testing::reference_aggregate;

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

/* 15 identical blocks: every channel carries the same Q16 RMS value. */
std::array<ReferenceBasicInput, basic_blocks_per_aggregate>
constant_blocks(std::uint64_t rms_q16, std::uint32_t frequency_millihz)
{
	std::array<ReferenceBasicInput, basic_blocks_per_aggregate> blocks{};
	for (auto &block : blocks) {
		block.rms_q16.fill(rms_q16);
		block.frequency_millihz = frequency_millihz;
		block.frequency_valid = true;
	}
	return blocks;
}

/**
 * Wire-image builder for one MTR2 record, defaulting to a valid 60 Hz
 * aggregate (15 blocks, 180 cycles). Tests override single fields to build
 * the malformed variants.
 */
struct AggregateSpec {
	std::uint32_t sequence = 1;
	std::uint32_t generation = 0x12345678u;
	std::uint32_t sample_rate_hz = 128'000;
	/* 15 cycle-defined blocks of ~25'600 samples at 128 kSPS; the odd
	 * total mirrors the varying per-block counts. */
	std::uint32_t sample_count = 384'015;
	std::uint32_t valid_mask = 0x7f;
	bool arithmetic_error = false;
	bool complete = true;
	bool frequency_valid = true;
	std::uint32_t first_basic_sequence = 100;
	std::uint32_t last_basic_sequence = 114;
	std::uint32_t basic_block_count = 15;
	std::uint32_t nominal_hz = 60;
	std::uint32_t cycle_count = 180;
	std::uint64_t first_sample_index = 0x230000010ull;
	std::array<std::int64_t, msap1::meter_channel_count> rms_micro_units{};
	std::uint32_t frequency_millihz = 60'000;
};

msap1::MeterRecord aggregate_record(const AggregateSpec &spec)
{
	msap1::MeterRecord record{};
	record.words[0] = msap1::meter_record_magic;
	record.words[1] = msap1::meter_aggregate_format;
	record.words[2] = msap1::meter_record_size;
	record.words[3] = spec.sequence;
	record.words[4] = spec.generation;
	record.words[5] = spec.sample_rate_hz;
	record.words[6] = spec.sample_count;
	record.words[7] = spec.valid_mask;
	/* bit1 (complete) is set on every record the PL emits; tests clear it
	 * only to prove the decoder refuses a self-declared partial. */
	record.words[8] = (spec.arithmetic_error ? (1u << 0) : 0u) |
			  (spec.complete ? (1u << 1) : 0u) |
			  (spec.frequency_valid ? (1u << 2) : 0u);
	record.words[9] = static_cast<std::uint32_t>(spec.first_sample_index);
	record.words[10] =
		static_cast<std::uint32_t>(spec.first_sample_index >> 32);
	record.words[13] = spec.basic_block_count | (spec.nominal_hz << 8) |
			   (spec.cycle_count << 16);
	record.words[14] = spec.first_basic_sequence;
	record.words[15] = spec.last_basic_sequence;
	for (std::size_t channel = 0;
	     channel != msap1::meter_channel_count; ++channel) {
		const auto bits = std::bit_cast<std::uint64_t>(
			spec.rms_micro_units[channel]);
		record.words[16 + channel * 2] =
			static_cast<std::uint32_t>(bits);
		record.words[17 + channel * 2] =
			static_cast<std::uint32_t>(bits >> 32);
	}
	record.words[32] = spec.frequency_millihz;
	/* AGG-v3 additions: interval last-sample anchor + line-line RMS. */
	record.words[36] =
		static_cast<std::uint32_t>(spec.first_sample_index + 384'014);
	record.words[37] = static_cast<std::uint32_t>(
		(spec.first_sample_index + 384'014) >> 32);
	record.words[38] = 12'000'000;
	record.words[39] = 11'000'000;
	record.words[40] = 13'000'000;
	return record;
}

/** Wire image for one M13 clock-aligned ten-minute fundamental record. */
struct TenMinuteSpec {
	std::uint32_t sequence = 9;
	std::uint32_t generation = 0x12345678u;
	std::uint32_t sample_rate_hz = 32'000;
	std::uint32_t sample_count = 19'200'000;
	std::uint32_t valid_mask = 0x7f;
	bool arithmetic_error = false;
	bool complete = true;
	bool time_aligned = true;
	bool contaminated = false;
	bool boundary_valid = true;
	std::uint32_t first_basic_sequence = 1'000;
	std::uint32_t last_basic_sequence = 3'999;
	std::uint32_t basic_block_count = 3'000;
	std::uint32_t nominal_hz = 60;
	std::uint32_t cycle_count = 36'000;
	std::uint64_t first_sample_index = 5'000'001ull;
	std::uint32_t overshoot_samples = 127;
	std::array<std::int64_t, msap1::meter_channel_count>
		rms_micro_units{};
};

msap1::MeterRecord ten_minute_record(const TenMinuteSpec &spec)
{
	msap1::MeterRecord record{};
	record.words[0] = msap1::meter_record_magic;
	record.words[1] = msap1::meter_ten_minute_format;
	record.words[2] = msap1::meter_record_size;
	record.words[3] = spec.sequence;
	record.words[4] = spec.generation;
	record.words[5] = spec.sample_rate_hz;
	record.words[6] = spec.sample_count;
	record.words[7] = spec.valid_mask;
	record.words[8] = (spec.arithmetic_error ? (1u << 0) : 0u) |
			  (spec.complete ? (1u << 1) : 0u) |
			  (spec.time_aligned ? (1u << 2) : 0u) |
			  (spec.contaminated ? (1u << 3) : 0u) |
			  (spec.boundary_valid ? (1u << 4) : 0u);
	record.words[9] = static_cast<std::uint32_t>(spec.first_sample_index);
	record.words[10] =
		static_cast<std::uint32_t>(spec.first_sample_index >> 32);
	record.words[13] = spec.basic_block_count | (spec.nominal_hz << 16);
	record.words[14] = spec.first_basic_sequence;
	record.words[15] = spec.last_basic_sequence;
	for (std::size_t channel = 0;
	     channel != msap1::meter_channel_count; ++channel) {
		const auto bits = std::bit_cast<std::uint64_t>(
			spec.rms_micro_units[channel]);
		record.words[16 + channel * 2] =
			static_cast<std::uint32_t>(bits);
		record.words[17 + channel * 2] =
			static_cast<std::uint32_t>(bits >> 32);
	}
	const auto actual_last =
		spec.first_sample_index + spec.sample_count - 1u;
	const auto target = actual_last - spec.overshoot_samples;
	record.words[36] = static_cast<std::uint32_t>(actual_last);
	record.words[37] = static_cast<std::uint32_t>(actual_last >> 32);
	record.words[38] = 208'000'000;
	record.words[39] = 207'000'000;
	record.words[40] = 209'000'000;
	record.words[41] = spec.cycle_count;
	record.words[42] = static_cast<std::uint32_t>(target);
	record.words[43] = static_cast<std::uint32_t>(target >> 32);
	record.words[44] = spec.overshoot_samples;
	return record;
}

/** Wire image for one M14 two-hour record built from twelve ten-minute images. */
struct TwoHourSpec {
	std::uint32_t sequence = 3;
	std::uint32_t generation = 0x12345678u;
	std::uint32_t sample_rate_hz = 32'000;
	std::uint32_t sample_count = 230'400'000;
	std::uint32_t valid_mask = 0x7f;
	bool arithmetic_error = false;
	bool complete = true;
	bool time_aligned = true;
	bool contaminated = false;
	bool boundary_valid = true;
	std::uint32_t first_ten_minute_sequence = 100;
	std::uint32_t last_ten_minute_sequence = 111;
	std::uint32_t ten_minute_count = 12;
	std::uint32_t nominal_hz = 60;
	std::uint32_t cycle_count = 432'000;
	std::uint64_t first_sample_index = 25'000'001ull;
	std::uint32_t overshoot_samples = 127;
	std::array<std::int64_t, msap1::meter_channel_count>
		rms_micro_units{};
};

msap1::MeterRecord two_hour_record(const TwoHourSpec &spec)
{
	msap1::MeterRecord record{};
	record.words[0] = msap1::meter_record_magic;
	record.words[1] = msap1::meter_two_hour_format;
	record.words[2] = msap1::meter_record_size;
	record.words[3] = spec.sequence;
	record.words[4] = spec.generation;
	record.words[5] = spec.sample_rate_hz;
	record.words[6] = spec.sample_count;
	record.words[7] = spec.valid_mask;
	record.words[8] = (spec.arithmetic_error ? (1u << 0) : 0u) |
			  (spec.complete ? (1u << 1) : 0u) |
			  (spec.time_aligned ? (1u << 2) : 0u) |
			  (spec.contaminated ? (1u << 3) : 0u) |
			  (spec.boundary_valid ? (1u << 4) : 0u);
	record.words[9] = static_cast<std::uint32_t>(spec.first_sample_index);
	record.words[10] =
		static_cast<std::uint32_t>(spec.first_sample_index >> 32);
	record.words[13] = spec.ten_minute_count | (spec.nominal_hz << 16);
	record.words[14] = spec.first_ten_minute_sequence;
	record.words[15] = spec.last_ten_minute_sequence;
	for (std::size_t channel = 0;
	     channel != msap1::meter_channel_count; ++channel) {
		const auto bits = std::bit_cast<std::uint64_t>(
			spec.rms_micro_units[channel]);
		record.words[16 + channel * 2] = static_cast<std::uint32_t>(bits);
		record.words[17 + channel * 2] =
			static_cast<std::uint32_t>(bits >> 32);
	}
	const auto actual_last =
		spec.first_sample_index + spec.sample_count - 1u;
	const auto target = actual_last - spec.overshoot_samples;
	record.words[36] = static_cast<std::uint32_t>(actual_last);
	record.words[37] = static_cast<std::uint32_t>(actual_last >> 32);
	record.words[38] = 208'000'000;
	record.words[39] = 207'000'000;
	record.words[40] = 209'000'000;
	record.words[41] = spec.cycle_count;
	record.words[42] = static_cast<std::uint32_t>(target);
	record.words[43] = static_cast<std::uint32_t>(target >> 32);
	record.words[44] = spec.overshoot_samples;
	return record;
}

/** Convert a completed long-interval wire image into an M15 live preview. */
msap1::MeterRecord open_interval_record(msap1::MeterRecord record,
	std::uint32_t format, std::uint64_t remaining_samples)
{
	record.words[1] = format;
	/* Clear COMPLETE and set OPEN_INTERVAL | NON_NORMATIVE while retaining
	 * aligned/boundary status from the completed-record fixture. */
	record.words[8] &= ~(1u << 1);
	record.words[8] |= (1u << 5) | (1u << 6);
	const auto actual_last =
		(static_cast<std::uint64_t>(record.words[37]) << 32) |
		record.words[36];
	const auto expected_end = actual_last + remaining_samples;
	record.words[42] = static_cast<std::uint32_t>(expected_end);
	record.words[43] = static_cast<std::uint32_t>(expected_end >> 32);
	record.words[44] = 0u;
	return record;
}

void open_intervals_are_non_normative_independent_views()
{
	auto registry = msap1::MeterDecoderRegistry::with_builtin_decoders();
	const auto ten = open_interval_record(ten_minute_record(TenMinuteSpec{}),
		msap1::meter_ten_minute_open_format, 96'000u);
	const auto ten_update = registry.decode(ten);
	require(ten_update.period == msap1::MeasurementPeriod::Min10Live &&
		ten_update.aggregate_timing &&
		ten_update.aggregate_timing->overshoot_samples == 0u &&
		ten_update.aggregate_timing->target_sample_index >
			ten.ten_minute_actual_last_sample_index(),
		"ten-minute live preview lost its open-window provenance");

	const auto two = open_interval_record(two_hour_record(TwoHourSpec{}),
		msap1::meter_two_hour_open_format, 19'200'000u);
	const auto two_update = registry.decode(two);
	require(two_update.period == msap1::MeasurementPeriod::Hour2Live &&
		two_update.aggregate_timing &&
		two_update.aggregate_timing->basic_block_count == 12u,
		"two-hour live preview decoded on the wrong independent view");

	msap1::MeterLatestStore store;
	store.apply(ten_update);
	store.apply(two_update);
	require(store.latest(msap1::MeasurementPeriod::Min10Live).has_value() &&
		store.latest(msap1::MeasurementPeriod::Hour2Live).has_value() &&
		!store.latest(msap1::MeasurementPeriod::Min10).has_value() &&
		!store.latest(msap1::MeasurementPeriod::Hour2).has_value(),
		"live previews replaced an authoritative completed interval");

	auto power = ten;
	power.words[1] = msap1::meter_ten_minute_open_power_format;
	require(registry.decode(power).period ==
			msap1::MeasurementPeriod::Min10Live,
		"open-window sibling decoded on the wrong period");

	auto missing_marker = ten;
	missing_marker.words[8] &= ~(1u << 6);
	require_throws([&] { (void)registry.decode(missing_marker); },
		"live preview without NON_NORMATIVE marker decoded");
}

void two_hour_decodes_cascaded_provenance()
{
	TwoHourSpec spec{};
	spec.rms_micro_units = {
		3'000'000, 3'100'000, 2'900'000, 0,
		120'000'000, 121'000'000, 122'000'000, 0,
	};
	auto registry = msap1::MeterDecoderRegistry::with_builtin_decoders();
	const auto update = registry.decode(two_hour_record(spec));
	require(update.period == msap1::MeasurementPeriod::Hour2 &&
		update.kind == msap1::RecordKind::fundamental &&
		update.fundamental.has_value() &&
		update.aggregate_timing.has_value(),
		"M14 record did not decode as a two-hour fundamental update");
	require(update.fundamental->voltage_ln.phase_a.value == 122'000'000 &&
		update.fundamental->current.neutral.valid() &&
		update.fundamental->current.neutral.value == 0 &&
		!update.fundamental->frequency.available(),
		"M14 values, valid zero, or frequency availability changed");
	const auto &timing = *update.aggregate_timing;
	require(timing.basic_block_count == 12 &&
		timing.cycle_count == 432'000 && timing.time_aligned &&
		!timing.contaminated && timing.boundary_valid &&
		timing.first_basic_sequence == 100 &&
		timing.last_basic_sequence == 111,
		"M14 cascaded timing provenance did not survive decoding");

	msap1::MeterLatestStore store;
	store.apply(update);
	const auto view = store.latest(msap1::MeasurementPeriod::Hour2);
	require(view && view->latest_sequence == spec.sequence &&
		view->aggregate_timing &&
		view->aggregate_timing->basic_block_count == 12,
		"two-hour update was not retained in its independent view");

	auto sibling = two_hour_record(spec);
	sibling.words[1] = msap1::meter_two_hour_power_format;
	sibling.words[16] = 123;
	require(registry.decode(sibling).period ==
			msap1::MeasurementPeriod::Hour2,
		"two-hour power sibling decoded on the wrong period");
	sibling.words[1] = msap1::meter_two_hour_phasor_format;
	require(registry.decode(sibling).period ==
			msap1::MeasurementPeriod::Hour2,
		"two-hour phasor sibling decoded on the wrong period");
	sibling.words[1] = msap1::meter_two_hour_unbalance_format;
	require(registry.decode(sibling).period ==
			msap1::MeasurementPeriod::Hour2,
		"two-hour unbalance sibling decoded on the wrong period");
}

void two_hour_rejects_incomplete_or_discontinuous_inputs()
{
	auto registry = msap1::MeterDecoderRegistry::with_builtin_decoders();
	auto wrong_count = TwoHourSpec{};
	wrong_count.ten_minute_count = 11;
	require_throws([&] { (void)registry.decode(two_hour_record(wrong_count)); },
		"two-hour result with eleven inputs decoded");
	auto wrong_span = TwoHourSpec{};
	wrong_span.last_ten_minute_sequence = 112;
	require_throws([&] { (void)registry.decode(two_hour_record(wrong_span)); },
		"two-hour result with a discontinuous input span decoded");
	auto incomplete = TwoHourSpec{};
	incomplete.complete = false;
	require_throws([&] { (void)registry.decode(two_hour_record(incomplete)); },
		"incomplete two-hour result decoded");
	auto wrong_boundary = two_hour_record(TwoHourSpec{});
	++wrong_boundary.words[44];
	require_throws([&] { (void)registry.decode(wrong_boundary); },
		"two-hour result with false boundary provenance decoded");
}

void ten_minute_decodes_boundary_provenance()
{
	TenMinuteSpec spec{};
	spec.rms_micro_units = {
		3'000'000, 3'100'000, 2'900'000, 0,
		120'000'000, 121'000'000, 122'000'000, 0,
	};
	const auto timestamp = std::chrono::system_clock::time_point{600s};
	auto registry = msap1::MeterDecoderRegistry::with_builtin_decoders();
	const auto update = registry.decode(ten_minute_record(spec), timestamp);

	require(update.period == msap1::MeasurementPeriod::Min10 &&
		update.kind == msap1::RecordKind::fundamental &&
		update.fundamental.has_value() &&
		update.aggregate_timing.has_value(),
		"M13 record did not decode as a ten-minute fundamental update");
	const auto &values = *update.fundamental;
	require(values.voltage_ln.phase_a.valid() &&
		values.voltage_ln.phase_a.value == 122'000'000 &&
		values.current.neutral.valid() &&
		values.current.neutral.value == 0,
		"M13 channel values or valid zero did not decode");
	require(!values.frequency.available(),
		"ten-minute informative frequency was advertised as valid");

	const auto &timing = *update.aggregate_timing;
	const auto actual_last =
		spec.first_sample_index + spec.sample_count - 1u;
	require(timing.basic_block_count == 3'000 &&
		timing.cycle_count == 36'000 && timing.time_aligned &&
		!timing.contaminated && timing.boundary_valid &&
		timing.target_sample_index ==
			actual_last - spec.overshoot_samples &&
		timing.overshoot_samples == spec.overshoot_samples,
		"M13 UTC-boundary provenance did not survive decoding");

	msap1::MeterLatestStore store;
	store.apply(update);
	const auto view = store.latest(msap1::MeasurementPeriod::Min10);
	require(view.has_value() && view->latest_sequence == spec.sequence &&
		view->aggregate_timing.has_value() &&
		view->aggregate_timing->time_aligned,
		"ten-minute update was not retained in its independent view");
}

void ten_minute_contamination_is_visible_but_invalid()
{
	TenMinuteSpec contaminated{};
	contaminated.rms_micro_units.fill(1'000'000);
	contaminated.contaminated = true;
	auto registry = msap1::MeterDecoderRegistry::with_builtin_decoders();
	const auto update = registry.decode(ten_minute_record(contaminated));
	require(update.aggregate_timing->contaminated &&
		update.fundamental->voltage_ln.phase_a.quality ==
			msap1::MeasurementQuality::invalid,
		"a startup-contaminated ten-minute interval appeared valid");

	/* Sibling records carry the same interval state in status bits even
	 * though their payload words are occupied by their own quantities. */
	auto power_record = ten_minute_record(contaminated);
	power_record.words[1] = msap1::meter_ten_minute_power_format;
	power_record.words[16] = 123;
	power_record.words[18] = 456;
	const auto power = registry.decode(power_record);
	require(power.period == msap1::MeasurementPeriod::Min10 &&
		power.power.has_value() &&
		power.power->active_power.phase_a.quality ==
			msap1::MeasurementQuality::invalid,
		"ten-minute sibling ignored contaminated interval status");
}

void ten_minute_rejects_inconsistent_boundaries()
{
	auto registry = msap1::MeterDecoderRegistry::with_builtin_decoders();
	require(registry.decode(ten_minute_record(TenMinuteSpec{}))
			.aggregate_timing.has_value(),
		"baseline ten-minute record did not decode");

	auto wrong_cycles = TenMinuteSpec{};
	wrong_cycles.cycle_count = 35'999;
	require_throws([&] {
		(void)registry.decode(ten_minute_record(wrong_cycles));
	}, "ten-minute record with an inconsistent cycle count decoded");

	auto wrong_span = TenMinuteSpec{};
	wrong_span.last_basic_sequence = 4'000;
	require_throws([&] {
		(void)registry.decode(ten_minute_record(wrong_span));
	}, "ten-minute record with a nonconsecutive basic span decoded");

	auto wrong_actual = ten_minute_record(TenMinuteSpec{});
	++wrong_actual.words[36];
	require_throws([&] {
		(void)registry.decode(wrong_actual);
	}, "ten-minute record with a false actual boundary decoded");

	auto wrong_overshoot = ten_minute_record(TenMinuteSpec{});
	++wrong_overshoot.words[44];
	require_throws([&] {
		(void)registry.decode(wrong_overshoot);
	}, "ten-minute record with a false boundary overshoot decoded");

	auto incomplete = TenMinuteSpec{};
	incomplete.complete = false;
	require_throws([&] {
		(void)registry.decode(ten_minute_record(incomplete));
	}, "an incomplete ten-minute aggregate decoded");
}

/* AGG-v3 (M11): the line-line words decode with pair validity. */
void aggregate_decodes_line_line()
{
	AggregateSpec spec{};
	spec.rms_micro_units.fill(1'000'000);
	const auto update = msap1::decode_aggregate_meter_record(
		aggregate_record(spec), std::chrono::system_clock::now());
	const auto &fundamental = *update.fundamental;
	require(fundamental.voltage_ll.phase_a.valid() &&
			fundamental.voltage_ll.phase_a.value == 12'000'000 &&
			fundamental.voltage_ll.phase_b.value == 11'000'000 &&
			fundamental.voltage_ll.phase_c.value == 13'000'000,
		"AGG-v3 line-line words 38..40 were not decoded");
}

/* The aggregate sibling records (M11) decode on the Cycles150_180 period
 * with the payload maps shared with the basic-period decoders. */
void aggregate_siblings_decode()
{
	msap1::MeterRecord record{};
	record.words[0] = msap1::meter_record_magic;
	record.words[1] = msap1::meter_aggregate_power_format;
	record.words[2] = msap1::meter_record_size;
	record.words[3] = 7;
	record.words[4] = 0x12345678u;
	record.words[5] = 128'000;
	record.words[6] = 384'015;
	record.words[7] = 0x7f;
	record.words[9] = 0x10;
	record.words[13] = 15u | (60u << 8) | (180u << 16);
	record.words[14] = 100;
	record.words[15] = 114;
	record.words[16] = 360; /* P_A pW */
	record.words[18] = 720; /* S_A pVA */
	record.words[20] = 500000;
	const auto update = msap1::decode_aggregate_power_meter_record(
		record, std::chrono::system_clock::now());
	require(update.period == msap1::MeasurementPeriod::Cycles150_180 &&
			update.kind == msap1::RecordKind::power &&
			update.sequence == 7 && update.power.has_value(),
		"AGG-POWER did not decode as an aggregate power update");
	require(update.power->active_power.phase_a.value == 360 &&
			update.power->power_factor.phase_a.value == 500000,
		"AGG-POWER payload map must match POWER-v1");

	record.words[1] = msap1::meter_aggregate_phasor_format;
	record.words[17] = 0; /* keep angle words clean */
	const auto phasor = msap1::decode_aggregate_phasor_meter_record(
		record, std::chrono::system_clock::now());
	require(phasor.period == msap1::MeasurementPeriod::Cycles150_180 &&
			phasor.phasor.has_value(),
		"AGG-PHASOR did not decode on the aggregate period");

	record.words[1] = msap1::meter_aggregate_unbalance_format;
	const auto unbalance = msap1::decode_aggregate_unbalance_meter_record(
		record, std::chrono::system_clock::now());
	require(unbalance.period == msap1::MeasurementPeriod::Cycles150_180 &&
			unbalance.unbalance.has_value(),
		"AGG-UNBAL did not decode on the aggregate period");
}

/*
 * Aggregating 15 identical blocks must reproduce the block exactly:
 * sqrt(15 v^2 / 15) = v with no rounding loss anywhere in the pipeline.
 */
void reference_reproduces_constant_inputs()
{
	const auto volts_q16 = std::uint64_t{230'000'000} << 16; /* 230 V */
	const auto aggregate =
		reference_aggregate(constant_blocks(volts_q16, 60'000));
	for (std::size_t channel = 0;
	     channel != msap1::meter_channel_count; ++channel) {
		require(aggregate.rms_q16[channel] == volts_q16,
			"a constant input did not aggregate to itself in Q16");
		require(aggregate.rms_micro_units[channel] == 230'000'000,
			"Q16 -> micro-unit conversion is not a floor by 2^16");
	}
	require(aggregate.frequency_valid &&
		aggregate.frequency_millihz == 60'000,
		"a constant frequency did not aggregate to itself");
}

/*
 * Deliberately varying inputs make arithmetic mistakes visible. For the
 * Q16 values 1000, 2000, ..., 15000:
 *   sum of squares = 1000^2 * (1^2 + ... + 15^2) = 1'240'000'000
 *   floor(/15)     = 82'666'666
 *   floor(sqrt)    = 9092        (9092^2 = 82'664'464 <= 82'666'666)
 * which differs from the arithmetic mean 8000, so a mean-of-values bug or
 * a wrong divisor cannot pass. The frequency vector exercises the floor of
 * the mean: fourteen 60'000 readings and one 60'007 average 60'000.466...
 */
void reference_matches_hand_computed_vectors()
{
	std::array<ReferenceBasicInput, basic_blocks_per_aggregate> blocks{};
	for (std::size_t index = 0; index != blocks.size(); ++index) {
		blocks[index].rms_q16.fill((index + 1) * std::uint64_t{1000});
		blocks[index].frequency_millihz = 60'000;
		blocks[index].frequency_valid = true;
	}
	blocks[4].frequency_millihz = 60'007;
	const auto aggregate = reference_aggregate(blocks);
	require(aggregate.rms_q16[0] == 9092,
		"varying inputs did not aggregate to floor(sqrt(mean(squares)))");
	require(aggregate.rms_micro_units[0] == 0,
		"a sub-micro-unit Q16 aggregate did not floor to zero");
	require(aggregate.frequency_valid &&
		aggregate.frequency_millihz == 60'000,
		"the frequency mean did not floor");

	/* One invalid basic reading invalidates the whole mean: the record
	 * then carries frequency 0 with the valid bit clear. */
	auto with_invalid = blocks;
	with_invalid[9].frequency_valid = false;
	const auto invalid = reference_aggregate(with_invalid);
	require(!invalid.frequency_valid && invalid.frequency_millihz == 0,
		"an invalid basic frequency survived into the aggregate");
}

/*
 * End-to-end 60 Hz: compute the expected aggregate from 15 varying basic
 * blocks with the reference, emit it as an MTR2 wire record exactly as the
 * PL would, and decode it back through the builtin registry.
 */
void decode_reference_built_record_60hz()
{
	std::array<ReferenceBasicInput, basic_blocks_per_aggregate> blocks{};
	for (std::size_t index = 0; index != blocks.size(); ++index) {
		auto &block = blocks[index];
		/* PL channel order: Ia, Ib, Ic, In, Vc, Vb, Va, ch7 = 0.
		 * In is a VALID zero (energized channel, no load). */
		block.rms_q16 = {
			(std::uint64_t{5'250'000} + index * 500) << 16,
			std::uint64_t{5'100'000} << 16,
			std::uint64_t{4'900'000} << 16,
			0,
			std::uint64_t{229'500'000} << 16,
			std::uint64_t{231'000'000} << 16,
			(std::uint64_t{230'000'000} + index * 1000) << 16,
			0,
		};
		block.frequency_millihz =
			59'998 + static_cast<std::uint32_t>(index % 5);
		block.frequency_valid = true;
	}
	const auto expected = reference_aggregate(blocks);

	AggregateSpec spec{};
	spec.sequence = 7;
	spec.rms_micro_units = expected.rms_micro_units;
	spec.frequency_millihz = expected.frequency_millihz;
	spec.frequency_valid = expected.frequency_valid;

	const auto timestamp = std::chrono::system_clock::time_point{123s};
	auto registry = msap1::MeterDecoderRegistry::with_builtin_decoders();
	const auto update = registry.decode(aggregate_record(spec), timestamp);

	require(update.period == msap1::MeasurementPeriod::Cycles150_180 &&
		update.kind == msap1::RecordKind::fundamental &&
		update.sequence == 7 &&
		update.configuration_generation == 0x12345678u &&
		update.fundamental.has_value(),
		"MTR2 did not decode as a 150/180-cycle fundamental update");
	require(!update.timing.has_value(),
		"an aggregate record fabricated basic BlockTiming");
	require(update.aggregate_timing.has_value(),
		"an aggregate record decoded without AggregateTiming");

	/* Same channel mapping and micro-unit encoding as MTR1. */
	const auto &values = *update.fundamental;
	require(values.voltage_ln.phase_a.value ==
			expected.rms_micro_units[6] &&
		values.voltage_ln.phase_b.value ==
			expected.rms_micro_units[5] &&
		values.voltage_ln.phase_c.value ==
			expected.rms_micro_units[4],
		"hardware Vc/Vb/Va order was not mapped to phase A/B/C");
	require(values.current.phase_a.value == expected.rms_micro_units[0] &&
		values.current.phase_b.value == expected.rms_micro_units[1] &&
		values.current.phase_c.value == expected.rms_micro_units[2],
		"aggregate currents were not decoded in channel order");
	require(values.current.neutral.valid() &&
		values.current.neutral.value == 0,
		"a valid zero aggregate current was confused with unavailable");
	/* The mean is informative only: the standardized Class A frequency
	 * product belongs to its own interval and is not implemented yet, so
	 * this reading must never advertise itself as valid. The value is
	 * still carried for diagnostics. */
	require(!values.frequency.valid() &&
		values.frequency.quality ==
			msap1::MeasurementQuality::unavailable &&
		values.frequency.value == expected.frequency_millihz,
		"the informative mean frequency was advertised as valid");
	require(values.frequency.measured_at == timestamp &&
		values.frequency.calculation_window.sample_count == 384'015 &&
		values.frequency.calculation_window.duration ==
			std::chrono::nanoseconds{3'000'117'187},
		"the window does not follow the actual total sample count");

	const auto &timing = *update.aggregate_timing;
	require(timing.sequence == 7 &&
		timing.configuration_generation == 0x12345678u,
		"aggregate timing identity does not match the record header");
	require(timing.first_sample_index == 0x230000010ull &&
		timing.sample_count == 384'015,
		"the aggregate sample range was not decoded from words 9/10/6");
	require(timing.first_basic_sequence == 100 &&
		timing.last_basic_sequence == 114,
		"the contributing basic sequence range was not decoded");
	require(timing.basic_block_count == 15 && timing.cycle_count == 180 &&
		timing.nominal_frequency == msap1::NominalFrequency::Hz60,
		"the composition word was not decoded");
	require(!timing.arithmetic_error && timing.frequency_valid,
		"aggregate status bits were not decoded");
	/* UTC state is stamped by the ingestor, never by the decoder. */
	require(timing.time_quality == msap1::TimeQuality::Unsynchronized &&
		!timing.utc_start.has_value() &&
		!timing.utc_uncertainty_ns.has_value(),
		"decoder fabricated UTC state that only the timebase knows");

	/* The latest store files the update under its own period and keeps
	 * the aggregate identity, without touching the Basic slot. */
	msap1::MeterLatestStore store;
	store.apply(update);
	const auto view =
		store.latest(msap1::MeasurementPeriod::Cycles150_180);
	require(view.has_value() && view->latest_sequence == 7 &&
		view->aggregate_timing.has_value() &&
		view->aggregate_timing->cycle_count == 180 &&
		!view->timing.has_value(),
		"the period view did not carry the aggregate identity");
	require(!store.latest(msap1::MeasurementPeriod::Basic).has_value(),
		"an aggregate update leaked into the Basic period");
}

/* 50 Hz variant (10-cycle basics -> 150 cycles) with an invalid mean
 * frequency: word 32 must be zero and the reading unavailable. */
void decode_reference_built_record_50hz()
{
	auto blocks =
		constant_blocks(std::uint64_t{229'000'000} << 16, 49'987);
	blocks[7].frequency_valid = false;
	const auto expected = reference_aggregate(blocks);
	require(!expected.frequency_valid &&
		expected.frequency_millihz == 0,
		"reference did not invalidate the 50 Hz frequency mean");

	AggregateSpec spec{};
	spec.sample_count = 384'000;
	spec.nominal_hz = 50;
	spec.cycle_count = 150;
	spec.first_basic_sequence = 1;
	spec.last_basic_sequence = 15;
	spec.rms_micro_units = expected.rms_micro_units;
	spec.frequency_millihz = expected.frequency_millihz;
	spec.frequency_valid = expected.frequency_valid;

	auto registry = msap1::MeterDecoderRegistry::with_builtin_decoders();
	const auto update = registry.decode(aggregate_record(spec));
	require(update.aggregate_timing.has_value(),
		"the 50 Hz aggregate did not decode");
	const auto &timing = *update.aggregate_timing;
	require(timing.nominal_frequency == msap1::NominalFrequency::Hz50 &&
		timing.cycle_count == 150 && timing.basic_block_count == 15,
		"the 50 Hz composition was not decoded");
	require(!timing.frequency_valid,
		"an invalid mean frequency was decoded as valid");
	require(update.fundamental->voltage_ln.phase_a.value == 229'000'000,
		"the 50 Hz aggregate voltage was not decoded");
	require(!update.fundamental->frequency.available() &&
		update.fundamental->frequency.value == 0,
		"an invalid mean frequency did not decode as unavailable");
}

/* A record flagging an internal PL overflow degrades quality without being
 * rejected: the record is still structurally valid. */
void decode_flags_arithmetic_error()
{
	AggregateSpec spec{};
	spec.arithmetic_error = true;
	spec.frequency_valid = false;
	spec.frequency_millihz = 0;
	auto registry = msap1::MeterDecoderRegistry::with_builtin_decoders();
	const auto update = registry.decode(aggregate_record(spec));
	require(update.aggregate_timing->arithmetic_error,
		"the arithmetic error flag was not decoded");
	require(update.fundamental->frequency.quality ==
			msap1::MeasurementQuality::unavailable,
		"the informative frequency must stay unavailable");
}

/*
 * Quality priority for aggregate RMS readings: a saturated aggregation
 * outranks the channel mask, because a saturated value published as valid
 * would hide the fault from every consumer.
 */
void decode_applies_rms_quality_priority()
{
	auto registry = msap1::MeterDecoderRegistry::with_builtin_decoders();

	AggregateSpec healthy{};
	healthy.rms_micro_units[6] = 229'000'000;
	const auto good = registry.decode(aggregate_record(healthy));
	require(good.fundamental->voltage_ln.phase_a.quality ==
			msap1::MeasurementQuality::valid,
		"a healthy aggregate channel was not valid");

	auto saturated = healthy;
	saturated.arithmetic_error = true;
	const auto bad = registry.decode(aggregate_record(saturated));
	require(bad.fundamental->voltage_ln.phase_a.quality ==
			msap1::MeasurementQuality::arithmetic_error,
		"an arithmetic error did not degrade the voltage quality");
	require(bad.fundamental->current.phase_a.quality ==
			msap1::MeasurementQuality::arithmetic_error,
		"an arithmetic error did not degrade the current quality");

	/* Hardware channel order is Ia, Ib, Ic, In, Vc, Vb, Va: clearing bit 6
	 * removes Va only. */
	auto masked = healthy;
	masked.valid_mask = 0x7f & ~(1u << 6);
	const auto partial = registry.decode(aggregate_record(masked));
	require(partial.fundamental->voltage_ln.phase_a.quality ==
			msap1::MeasurementQuality::unavailable,
		"a masked-out channel was not unavailable");
	require(partial.fundamental->voltage_ln.phase_b.quality ==
			msap1::MeasurementQuality::valid,
		"masking one channel disturbed another");
}

/*
 * Malformed aggregation identities must never silently become valid
 * aggregates: the decoder rejects every shape the PL cannot emit, exactly
 * like the hardened MTR1 rules.
 */
void decode_rejects_malformed_aggregates()
{
	auto registry = msap1::MeterDecoderRegistry::with_builtin_decoders();
	require(registry.decode(aggregate_record(AggregateSpec{}))
			.aggregate_timing.has_value(),
		"the baseline aggregate record did not decode");

	/* The tier is DEFINED as 15 basic blocks; 14 is a partial. */
	auto fourteen_blocks = AggregateSpec{};
	fourteen_blocks.basic_block_count = 14;
	require_throws([&] {
		(void)registry.decode(aggregate_record(fourteen_blocks));
	}, "a 14-block aggregate decoded");

	/* 15 complete 60 Hz blocks close exactly 180 cycles, never 150. */
	auto wrong_cycles = AggregateSpec{};
	wrong_cycles.cycle_count = 150;
	require_throws([&] {
		(void)registry.decode(aggregate_record(wrong_cycles));
	}, "a 60 Hz aggregate with 150 cycles decoded");

	/* The nominal is configuration and can only be 50 or 60. */
	auto nominal_55 = AggregateSpec{};
	nominal_55.nominal_hz = 55;
	nominal_55.cycle_count = 165;
	require_throws([&] {
		(void)registry.decode(aggregate_record(nominal_55));
	}, "a 55 Hz nominal decoded");

	/* An aggregate of zero samples has no measurement in it. */
	auto empty = AggregateSpec{};
	empty.sample_count = 0;
	require_throws([&] {
		(void)registry.decode(aggregate_record(empty));
	}, "a zero-sample aggregate decoded");

	/* Only complete 15-block intervals are ever published, so a record
	 * that declares itself incomplete is corruption, not a partial to be
	 * salvaged. */
	auto incomplete = AggregateSpec{};
	incomplete.complete = false;
	require_throws([&] {
		(void)registry.decode(aggregate_record(incomplete));
	}, "an aggregate marked incomplete decoded");

	/* The basic sequence span must describe exactly 15 consecutive
	 * blocks: 100..114 is 15 blocks, 100..115 is 16. */
	auto wide_span = AggregateSpec{};
	wide_span.last_basic_sequence = 115;
	require_throws([&] {
		(void)registry.decode(aggregate_record(wide_span));
	}, "a 16-block basic sequence span decoded");

	auto narrow_span = AggregateSpec{};
	narrow_span.last_basic_sequence = 113;
	require_throws([&] {
		(void)registry.decode(aggregate_record(narrow_span));
	}, "a 14-block basic sequence span decoded");

	/* The span is modular: 0xFFFFFFF8..0x00000006 is 15 consecutive
	 * blocks across the uint32 wrap and must be accepted unchanged. */
	auto wrapped = AggregateSpec{};
	wrapped.first_basic_sequence = 0xFFFFFFF8u;
	wrapped.last_basic_sequence = 0x00000006u;
	const auto wrapped_update = registry.decode(aggregate_record(wrapped));
	require(wrapped_update.aggregate_timing.has_value() &&
		wrapped_update.aggregate_timing->first_basic_sequence ==
			0xFFFFFFF8u &&
		wrapped_update.aggregate_timing->last_basic_sequence ==
			0x00000006u,
		"a wrapped basic sequence span was rejected");

	/* first_sample_index + sample_count must stay inside the 64-bit
	 * conversion counter. */
	auto overflowing = AggregateSpec{};
	overflowing.first_sample_index =
		std::numeric_limits<std::uint64_t>::max() - 1000;
	require_throws([&] {
		(void)registry.decode(aggregate_record(overflowing));
	}, "an overflowing aggregate sample range decoded");

	/* Zero is unreachable on the free-running conversion counter, the same
	 * as for a basic block. An aggregate seeded on a block whose index was
	 * zeroed inherits the zero, so it must be rejected rather than
	 * published with a UTC label anchored at the start of capture. */
	auto zero_index = AggregateSpec{};
	zero_index.first_sample_index = 0;
	require_throws([&] {
		(void)registry.decode(aggregate_record(zero_index));
	}, "a zero aggregate first-sample index decoded");
}

} // namespace

int main()
{
	try {
		aggregate_decodes_line_line();
		aggregate_siblings_decode();
		ten_minute_decodes_boundary_provenance();
		ten_minute_contamination_is_visible_but_invalid();
		ten_minute_rejects_inconsistent_boundaries();
		two_hour_decodes_cascaded_provenance();
		two_hour_rejects_incomplete_or_discontinuous_inputs();
		open_intervals_are_non_normative_independent_views();
		reference_reproduces_constant_inputs();
		reference_matches_hand_computed_vectors();
		decode_reference_built_record_60hz();
		decode_reference_built_record_50hz();
		decode_flags_arithmetic_error();
		decode_applies_rms_quality_priority();
		decode_rejects_malformed_aggregates();
		std::cout << "aggregate decode tests passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "aggregate decode test failed: " << error.what()
			  << '\n';
		return 1;
	}
}
