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
 * MTR2 (0x00020001) aggregate-record tests: the test-only reference
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
	/* bit1 (complete) is always set: the PL never emits partials. */
	record.words[8] = (spec.arithmetic_error ? (1u << 0) : 0u) |
			  (1u << 1) |
			  (spec.frequency_valid ? (1u << 2) : 0u);
	record.words[9] = spec.first_basic_sequence;
	record.words[10] = spec.last_basic_sequence;
	record.words[11] = spec.basic_block_count | (spec.nominal_hz << 8) |
			   (spec.cycle_count << 16);
	record.words[12] = static_cast<std::uint32_t>(spec.first_sample_index);
	record.words[13] =
		static_cast<std::uint32_t>(spec.first_sample_index >> 32);
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
	return record;
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
	require(values.frequency.valid() &&
		values.frequency.value == expected.frequency_millihz,
		"the aggregate mean frequency was not decoded as valid");
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
		"the aggregate sample range was not decoded from words 12/13/6");
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
			msap1::MeasurementQuality::arithmetic_error,
		"an arithmetic error did not degrade the frequency quality");
}

/*
 * Malformed aggregation identities must never silently become valid
 * aggregates: the decoder rejects every shape the PL cannot emit, exactly
 * like the hardened v2 rules.
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

	/* first_sample_index + sample_count must stay inside the 64-bit
	 * conversion counter. */
	auto overflowing = AggregateSpec{};
	overflowing.first_sample_index =
		std::numeric_limits<std::uint64_t>::max() - 1000;
	require_throws([&] {
		(void)registry.decode(aggregate_record(overflowing));
	}, "an overflowing aggregate sample range decoded");
}

} // namespace

int main()
{
	try {
		reference_reproduces_constant_inputs();
		reference_matches_hand_computed_vectors();
		decode_reference_built_record_60hz();
		decode_reference_built_record_50hz();
		decode_flags_arithmetic_error();
		decode_rejects_malformed_aggregates();
		std::cout << "aggregate decode tests passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "aggregate decode test failed: " << error.what()
			  << '\n';
		return 1;
	}
}
