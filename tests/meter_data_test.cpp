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

msap1::MeterRecord power_record()
{
	msap1::MeterRecord record{};
	record.words[0] = msap1::meter_record_magic;
	record.words[1] = msap1::meter_power_format;
	record.words[2] = msap1::meter_record_size;
	record.words[3] = 42;
	record.words[4] = 0x12345678;
	record.words[5] = 32000;
	record.words[6] = 6400;
	record.words[7] = 0x7f;
	record.words[9] = 0x00000010u;
	record.words[13] = 60u | (12u << 8) | (1u << 16);
	/* Phase A: import; B: export (sign pin); C: zero S -> PF undefined. */
	record.words[16] = 360;  record.words[17] = 0;      /* P_A = 360 pW */
	record.words[18] = 720;  record.words[19] = 0;      /* S_A */
	record.words[20] = 500000;                          /* PF_A = 0.5 */
	const auto negative =
		static_cast<std::uint64_t>(std::int64_t{-180});
	record.words[21] = static_cast<std::uint32_t>(negative);
	record.words[22] = static_cast<std::uint32_t>(negative >> 32);
	record.words[23] = 360;  record.words[24] = 0;
	record.words[25] = static_cast<std::uint32_t>(-500000);
	/* words 26..30 stay zero: S_C = 0. */
	record.words[31] = 180;  record.words[32] = 0;
	record.words[33] = 1080; record.words[34] = 0;
	record.words[35] = 166666;
	for (std::size_t lane = 0; lane != 7; ++lane)
		record.words[36 + lane] = 14142 + lane;
	return record;
}

void decode_power_record_pins()
{
	const auto timestamp = std::chrono::system_clock::time_point{124s};
	auto registry = msap1::MeterDecoderRegistry::with_builtin_decoders();
	const auto update = registry.decode(power_record(), timestamp);
	require(update.period == msap1::MeasurementPeriod::Basic &&
			update.kind == msap1::RecordKind::power &&
			update.sequence == 42 && update.power.has_value(),
		"POWER-v1 did not decode as a basic power update");
	const auto &values = *update.power;
	require(values.active_power.phase_a.value == 360 &&
			values.apparent_power.phase_a.value == 720 &&
			values.power_factor.phase_a.value == 500000 &&
			values.power_factor.phase_a.valid(),
		"phase A power words");
	require(values.active_power.phase_b.value == -180 &&
			values.power_factor.phase_b.value == -500000,
		"phase B sign extension");
	require(values.apparent_power.phase_c.value == 0 &&
			!values.power_factor.phase_c.valid(),
		"PF is unavailable when S is zero");
	require(values.total_active_power.value == 180 &&
			values.total_apparent_power.value == 1080 &&
			values.total_power_factor.value == 166666,
		"totals decode");
	require(values.current_crest.phase_a.value == 14142 &&
			values.voltage_crest.phase_a.value == 14142 + 6 &&
			values.voltage_crest.phase_c.value == 14142 + 4,
		"crest lanes map hardware order to phases");
}

/* PHASOR-v1 fixture: phase A lagging 30 deg (Q1 positive), phase B
 * leading (Q1 negative), phase C undefined (S1 = 0, nature 0). Flags
 * word 51: natures A=2 B=3 C=0, total=2, reference valid (bit 8). */
msap1::MeterRecord phasor_record()
{
	msap1::MeterRecord record{};
	record.words[0] = msap1::meter_record_magic;
	record.words[1] = msap1::meter_phasor_format;
	record.words[2] = msap1::meter_record_size;
	record.words[3] = 42;
	record.words[4] = 0x12345678;
	record.words[5] = 32000;
	record.words[6] = 6400;
	record.words[7] = 0x7f;
	record.words[9] = 0x00000010u;
	record.words[13] = 60u | (12u << 8) | (1u << 16);
	/* Per-lane fundamentals + angles: lanes Ia..In = 0..3, Vc/Vb/Va =
	 * 4/5/6 (Va angle must be 0 by the reference convention). */
	for (std::size_t lane = 0; lane != 7; ++lane) {
		record.words[16 + lane * 2] =
			lane == 3 ? 0u : 1'000'000u + static_cast<std::uint32_t>(lane);
		record.words[17 + lane * 2] = static_cast<std::uint32_t>(
			lane == 6 ? 0 : 330000 + lane);
	}
	/* VLL pairs AB/BC/CA. */
	record.words[30] = 2'000'000; record.words[31] = 30000;
	record.words[32] = 2'000'001;
	record.words[33] = 270000;
	record.words[34] = 2'000'002; record.words[35] = 150000;
	/* Displacement angles: A 30 deg, B 345 deg (a 15-degree lead), C 0. */
	record.words[36] = 30000;
	record.words[37] = 345000;
	/* Q1: A positive, B negative, C zero. */
	record.words[39] = 500; record.words[40] = 0;
	signed64(record, 41, -250);
	/* Q1 total. */
	record.words[45] = 250; record.words[46] = 0;
	/* Displacement PF: A 0.866, B -0.965, C 0 (undefined). */
	record.words[47] = 866025;
	record.words[48] = static_cast<std::uint32_t>(-965925);
	record.words[50] = 900000;
	/* Natures A=lagging B=leading C=undefined, total=lagging, ref ok. */
	record.words[51] = (2u << 0) | (3u << 2) | (0u << 4) | (2u << 6) |
			   (1u << 8);
	/* P1: A 866, B -400, C 0; total 466. */
	record.words[52] = 866; record.words[53] = 0;
	signed64(record, 54, -400);
	record.words[58] = 466; record.words[59] = 0;
	return record;
}

void decode_phasor_record_pins()
{
	const auto timestamp = std::chrono::system_clock::time_point{125s};
	auto registry = msap1::MeterDecoderRegistry::with_builtin_decoders();
	const auto update = registry.decode(phasor_record(), timestamp);
	require(update.period == msap1::MeasurementPeriod::Basic &&
			update.kind == msap1::RecordKind::phasor &&
			update.sequence == 42 && update.phasor.has_value(),
		"PHASOR-v1 did not decode as a basic phasor update");
	const auto &values = *update.phasor;
	require(!values.phasor_invalid && values.angle_reference_valid,
		"clean record flags");
	require(values.fundamental_voltage.phase_a.value == 1'000'006 &&
			values.fundamental_voltage.phase_c.value == 1'000'004 &&
			values.fundamental_current.phase_a.value == 1'000'000 &&
			values.fundamental_current.neutral.value == 0,
		"fundamental lanes map hardware order to phases");
	require(values.voltage_angle.phase_a.value == 0 &&
			values.voltage_angle.phase_a.valid() &&
			values.current_angle.phase_a.value == 330000 &&
			values.voltage_angle.phase_c.value == 330004,
		"angle words decode in the [0, 360000) convention");
	require(!values.current_angle.neutral.valid(),
		"a zero-fundamental lane has no meaningful angle");
	require(values.fundamental_voltage_ll.phase_a.value == 2'000'000 &&
			values.voltage_ll_angle.phase_b.value == 270000,
		"line-line phasor words");
	require(values.displacement_angle.phase_a.value == 30000 &&
			values.displacement_angle.phase_b.value == 345000,
		"displacement angles");
	require(values.reactive_power.phase_a.value == 500 &&
			values.reactive_power.phase_b.value == -250 &&
			values.total_reactive_power.value == 250,
		"Q1 decodes signed with arithmetic totals");
	require(values.fundamental_active_power.phase_a.value == 866 &&
			values.fundamental_active_power.phase_b.value == -400 &&
			values.total_fundamental_active_power.value == 466,
		"P1 decodes signed");
	require(values.displacement_power_factor.phase_a.value == 866025 &&
			values.displacement_power_factor.phase_b.value ==
				-965925 &&
			values.displacement_power_factor.phase_a.valid(),
		"displacement PF decodes signed");
	require(!values.displacement_power_factor.phase_c.valid() &&
			values.load_nature.phase_c ==
				msap1::LoadNature::undefined,
		"S1 = 0 phase: dPF unavailable, nature undefined");
	require(values.load_nature.phase_a == msap1::LoadNature::lagging &&
			values.load_nature.phase_b == msap1::LoadNature::leading &&
			values.total_load_nature == msap1::LoadNature::lagging,
		"load natures unpack from the flags word");

	/* The block-invalid status bit downgrades every reading. */
	auto poisoned = phasor_record();
	poisoned.words[8] = 0x2u;
	const auto bad = registry.decode(poisoned, timestamp);
	require(bad.phasor->phasor_invalid &&
			bad.phasor->reactive_power.phase_a.quality ==
				msap1::MeasurementQuality::invalid &&
			bad.phasor->voltage_angle.phase_a.quality ==
				msap1::MeasurementQuality::invalid,
		"phasor-invalid block decodes as invalid, not silently valid");
}

/* UNBALANCE-v1 fixture: V set with a zero-magnitude zero-sequence (angle
 * gating), a dominant positive sequence and a 2% negative sequence; I
 * ratios flagged invalid (|I1| = 0 upstream). Flags: V valid (bit 0),
 * reference valid (bit 8), I NOT valid. */
msap1::MeterRecord unbalance_record()
{
	msap1::MeterRecord record{};
	record.words[0] = msap1::meter_record_magic;
	record.words[1] = msap1::meter_unbalance_format;
	record.words[2] = msap1::meter_record_size;
	record.words[3] = 42;
	record.words[4] = 0x12345678;
	record.words[5] = 32000;
	record.words[6] = 6400;
	record.words[7] = 0x7f;
	record.words[9] = 0x00000010u;
	record.words[13] = 60u | (12u << 8) | (1u << 16);
	/* V0 = 0 @ 0; V1 = 1000000 @ 0; V2 = 20000 @ 270 deg. */
	record.words[16] = 0; record.words[17] = 0;
	record.words[18] = 1'000'000; record.words[19] = 0;
	record.words[20] = 20'000;
	record.words[21] = 270000;
	/* I components present but the I ratios are undefined. */
	record.words[22] = 5; record.words[23] = 15000;
	record.words[24] = 0; record.words[25] = 0;
	record.words[26] = 7;
	record.words[27] = 315000;
	record.words[28] = 0;      /* V0/V1 */
	record.words[29] = 20000;  /* UNBL_V = 2% */
	record.words[30] = 0;
	record.words[31] = 0;
	record.words[32] = (1u << 0) | (1u << 8);  /* V valid + ref, I not */
	return record;
}

void decode_unbalance_record_pins()
{
	const auto timestamp = std::chrono::system_clock::time_point{126s};
	auto registry = msap1::MeterDecoderRegistry::with_builtin_decoders();
	const auto update = registry.decode(unbalance_record(), timestamp);
	require(update.period == msap1::MeasurementPeriod::Basic &&
			update.kind == msap1::RecordKind::unbalance &&
			update.sequence == 42 && update.unbalance.has_value(),
		"UNBALANCE-v1 did not decode as a basic unbalance update");
	const auto &values = *update.unbalance;
	require(!values.phasor_invalid && values.angle_reference_valid,
		"clean record flags");
	require(values.voltage_positive_sequence.value == 1'000'000 &&
			values.voltage_negative_sequence.value == 20'000 &&
			values.voltage_negative_angle.value == 270000,
		"voltage sequence components decode");
	require(!values.voltage_zero_angle.valid(),
		"a zero-magnitude component has no meaningful angle");
	require(values.voltage_unbalance.value == 20000 &&
			values.voltage_unbalance.valid() &&
			values.voltage_zero_ratio.valid(),
		"voltage ratios valid under the flag");
	require(!values.current_unbalance.valid() &&
			!values.current_zero_ratio.valid(),
		"current ratios unavailable when |I1| = 0 upstream");
	require(values.current_zero_sequence.value == 5 &&
			values.current_zero_angle.value == 15000 &&
			values.current_negative_angle.value == 315000,
		"current components decode in the [0, 360000) convention");

	/* The block-invalid status bit downgrades every reading. */
	auto poisoned = unbalance_record();
	poisoned.words[8] = 0x2u;
	const auto bad = registry.decode(poisoned, timestamp);
	require(bad.unbalance->phasor_invalid &&
			bad.unbalance->voltage_unbalance.quality ==
				msap1::MeasurementQuality::invalid,
		"phasor-invalid block decodes as invalid");
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
	decode_power_record_pins();
	decode_phasor_record_pins();
	decode_unbalance_record_pins();
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
