/* Pin M17 ENERGY-v1 atomic assembly and DEMAND-v1 decode to the wire map. */

#include "msap1/meter/energy_demand.hpp"

#include <bit>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace {

int failures = 0;

void require(bool condition, const char *message)
{
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		++failures;
	}
}

void write_u64(msap1::MeterRecord &record, std::size_t word,
	std::uint64_t value)
{
	record.words[word] = static_cast<std::uint32_t>(value);
	record.words[word + 1] = static_cast<std::uint32_t>(value >> 32);
}

void write_s64(msap1::MeterRecord &record, std::size_t word,
	std::int64_t value)
{
	write_u64(record, word, std::bit_cast<std::uint64_t>(value));
}

msap1::MeterRecord energy_record(std::uint32_t sequence, std::uint8_t part)
{
	msap1::MeterRecord record{};
	record.words[0] = msap1::meter_record_magic;
	record.words[1] = msap1::meter_energy_format;
	record.words[2] = msap1::meter_record_size;
	record.words[3] = sequence;
	record.words[4] = 0x12345678u;
	record.words[5] = 32000u;
	record.words[6] = 6400u;
	record.words[7] = 0x7fu;
	record.words[8] = (1u << 1) | (1u << 4); // complete, discontinuity sticky
	write_u64(record, 9, 1000u + sequence * 6400u);
	record.words[13] = part | (2u << 2) | (1u << 4) | (0xfu << 8);
	write_u64(record, 14, 7399u + sequence * 6400u);
	constexpr std::uint64_t above_javascript_safe_integer =
		9007199254740993ULL;
	if (part == msap1::meter_energy_part_summary) {
		for (const auto base : {msap1::meter_energy_summary_import_word,
			msap1::meter_energy_summary_export_word,
			msap1::meter_energy_summary_apparent_word})
			for (std::size_t index = 0; index < 4; ++index)
				write_u64(record, base + index * 2,
					above_javascript_safe_integer + base + index);
	} else {
		for (const auto base : msap1::meter_energy_quadrant_words)
			for (std::size_t index = 0; index < 4; ++index)
				write_u64(record, base + index * 2,
					above_javascript_safe_integer + base + index);
	}
	write_u64(record, msap1::meter_energy_session_word,
		0xfedcba9876543210ULL);
	write_u64(record, msap1::meter_energy_accepted_samples_word, 123456u);
	write_u64(record, msap1::meter_energy_skipped_samples_word, 789u);
	record.words[msap1::meter_energy_accepted_blocks_word] = 42u;
	record.words[msap1::meter_energy_skipped_blocks_word] = 3u;
	return record;
}

msap1::MeterRecord demand_record(bool contaminated = false)
{
	msap1::MeterRecord record{};
	record.words[0] = msap1::meter_record_magic;
	record.words[1] = msap1::meter_demand_format;
	record.words[2] = msap1::meter_record_size;
	record.words[3] = 77u;
	record.words[4] = 0x12345678u;
	record.words[5] = 32000u;
	record.words[6] = 19200000u;
	record.words[7] = 0x7fu;
	record.words[8] = (1u << 1) | (1u << 2) | (1u << 4) |
		(static_cast<std::uint32_t>(contaminated) << 3);
	write_u64(record, 9, 1000000u);
	record.words[13] = msap1::meter_demand_interval_seconds | (0xfu << 16);
	write_u64(record, msap1::meter_demand_last_sample_word, 20199999u);
	constexpr std::int64_t signed_value = -9007199254740993LL;
	for (std::size_t index = 0; index < 4; ++index) {
		write_s64(record, msap1::meter_demand_current_word + index * 2,
			signed_value + static_cast<std::int64_t>(index));
		write_u64(record, msap1::meter_demand_import_peak_word + index * 2,
			9007199254741000ULL + index);
		write_u64(record, msap1::meter_demand_export_peak_word + index * 2,
			9007199254742000ULL + index);
		write_u64(record,
			msap1::meter_demand_import_peak_anchor_word + index * 2,
			100u + index);
		write_u64(record,
			msap1::meter_demand_export_peak_anchor_word + index * 2,
			200u + index);
	}
	write_u64(record, msap1::meter_demand_session_word,
		0xfedcba9876543210ULL);
	write_u64(record, msap1::meter_demand_target_sample_word, 20200000u);
	record.words[msap1::meter_demand_source_interval_count_word] = 3000u;
	record.words[msap1::meter_demand_source_status_word] = record.words[8];
	return record;
}

template<typename Function>
void rejects(Function function, const char *message)
{
	try {
		function();
	} catch (const std::invalid_argument &) {
		return;
	}
	require(false, message);
}

void test_quadrant_type()
{
	using Q = msap1::EnergyQuadrant;
	static_assert(msap1::classify_energy_quadrant(1, 1) == Q::quadrant_i);
	static_assert(msap1::classify_energy_quadrant(-1, 1) == Q::quadrant_ii);
	static_assert(msap1::classify_energy_quadrant(-1, -1) == Q::quadrant_iii);
	static_assert(msap1::classify_energy_quadrant(1, -1) == Q::quadrant_iv);
	static_assert(msap1::classify_energy_quadrant(0, 1) == Q::quadrant_i);
	static_assert(msap1::classify_energy_quadrant(0, -1) == Q::quadrant_iv);
	static_assert(msap1::classify_energy_quadrant(1, 0) == Q::none);
}

void test_energy_atomic_family()
{
	auto summary = energy_record(9u, msap1::meter_energy_part_summary);
	auto quadrants = energy_record(9u, msap1::meter_energy_part_quadrants);
	msap1::EnergyFamilyAssembler assembler;
	auto first = assembler.accept(summary);
	require(!first.completed && assembler.pending_parts() == 1u,
		"summary alone must remain unpublished");
	auto duplicate = assembler.accept(summary);
	require(duplicate.duplicate_part && !duplicate.completed &&
		assembler.pending_parts() == 1u,
		"byte-identical duplicate part must be idempotent");
	auto complete = assembler.accept(quadrants);
	require(complete.completed.has_value() && assembler.pending_parts() == 0u,
		"matching summary/quadrant parts must complete atomically");
	const auto &energy = *complete.completed;
	require(energy.session_id == 0xfedcba9876543210ULL &&
		energy.accepted_samples == 123456u && energy.skipped_samples == 789u &&
		energy.discontinuity,
		"ENERGY provenance decode");
	require(energy.active_import.phase_a.value ==
			static_cast<std::int64_t>(9007199254740993ULL +
				msap1::meter_energy_summary_import_word) &&
		energy.reactive(msap1::EnergyQuadrant::quadrant_iv).total.value ==
			static_cast<std::int64_t>(9007199254740993ULL +
				msap1::meter_energy_quadrant_words[3] + 3u),
		"ENERGY preserves exact counters above JavaScript safe integer range");

	(void)assembler.accept(energy_record(10u,
		msap1::meter_energy_part_summary));
	auto dropped = assembler.accept(energy_record(11u,
		msap1::meter_energy_part_summary));
	require(dropped.incomplete_families == 1u && !dropped.completed,
		"new identity must discard one incomplete ENERGY family");

	auto reserved = summary;
	reserved.words[63] = 1u;
	rejects([&] { (void)msap1::decode_energy_identity(reserved); },
		"ENERGY reserved tail word accepted");
	auto mismatched = quadrants;
	write_u64(mismatched, msap1::meter_energy_session_word, 123u);
	rejects([&] { (void)msap1::decode_energy_family(summary, mismatched); },
		"ENERGY mismatched session identity accepted");
}

void test_demand_decode()
{
	const auto update = msap1::decode_demand_meter_record(demand_record());
	require(update.kind == msap1::RecordKind::demand && update.demand,
		"DEMAND typed update route");
	require(update.demand->current_active.phase_a.value ==
			-9007199254740993LL &&
		update.demand->current_active.phase_a.quality ==
			msap1::MeasurementQuality::valid,
		"DEMAND preserves signed current above JavaScript safe integer range");
	require(update.demand->import_peak.total.value ==
			static_cast<std::int64_t>(9007199254741003ULL) &&
		update.demand->export_peak_sample.total == 203u,
		"DEMAND peak magnitude and anchor decode");

	const auto contaminated =
		msap1::decode_demand_meter_record(demand_record(true));
	require(contaminated.demand->current_active.phase_a.quality ==
			msap1::MeasurementQuality::invalid &&
		contaminated.demand->import_peak.phase_a.quality ==
			msap1::MeasurementQuality::valid,
		"contamination invalidates current demand without erasing peaks");
	auto reserved = demand_record();
	reserved.words[62] = 1u;
	rejects([&] { (void)msap1::decode_demand_meter_record(reserved); },
		"DEMAND reserved tail word accepted");
}

} // namespace

int main()
{
	test_quadrant_type();
	test_energy_atomic_family();
	test_demand_decode();
	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
