/* Pin M16 HARMONIC-v1 decode and 42-record atomic assembly to the PL map. */

#include "msap1/meter/harmonic_spectrum.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <vector>

namespace {

int failures = 0;

void require(bool condition, const char *message)
{
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		++failures;
	}
}

msap1::MeterRecord make_record(std::uint32_t sequence, std::uint8_t channel,
			       std::uint8_t chunk)
{
	msap1::MeterRecord record{};
	record.words[0] = msap1::meter_record_magic;
	record.words[1] = msap1::meter_harmonic_format;
	record.words[2] = msap1::meter_record_size;
	record.words[3] = sequence;
	record.words[4] = 0x12345678u;
	record.words[5] = 128000u;
	record.words[6] = 25600u;
	record.words[7] = 0x7fu;
	/* complete, grid locked, conditioner/FFT valid, full range */
	record.words[8] = 0x3eu;
	record.words[9] = 0x89abcdefu;
	record.words[10] = 0x00000012u;
	const auto first_order =
		static_cast<std::uint8_t>(chunk * msap1::harmonic_orders_per_record + 1);
	const auto count = static_cast<std::uint8_t>(std::min<std::size_t>(
		msap1::harmonic_orders_per_record,
		msap1::harmonic_max_order - first_order + 1));
	record.words[13] = channel | (static_cast<std::uint32_t>(chunk) << 3) |
		(static_cast<std::uint32_t>(first_order) << 7) |
		(static_cast<std::uint32_t>(count) << 15) |
		(static_cast<std::uint32_t>(
			 msap1::harmonic_chunks_per_channel) << 20) |
		(static_cast<std::uint32_t>(msap1::harmonic_max_order) << 24);
	record.words[14] = 50001u;
	record.words[15] = 127u | (50u << 8) | (10u << 16) | (3u << 24);
	for (std::size_t index = 0; index < count; ++index) {
		const auto order = first_order + index;
		const std::uint64_t magnitude =
			static_cast<std::uint64_t>(channel) * 1000000u + order;
		const std::uint64_t angle = order * 1000u;
		const std::uint64_t packed = magnitude | (angle << 40) |
			(std::uint64_t{1} << 60) | (std::uint64_t{1} << 61);
		record.words[16 + index * 2] = static_cast<std::uint32_t>(packed);
		record.words[17 + index * 2] =
			static_cast<std::uint32_t>(packed >> 32);
	}
	return record;
}

msap1::MeterRecord make_aggregate_record(std::uint32_t sequence,
					 std::uint8_t channel,
					 std::uint8_t chunk)
{
	msap1::MeterRecord record{};
	record.words[0] = msap1::meter_record_magic;
	record.words[1] = msap1::meter_harmonic_aggregate_format;
	record.words[2] = msap1::meter_record_size;
	record.words[3] = sequence;
	record.words[4] = 0x12345678u;
	record.words[5] = 32000u;
	record.words[6] = 96000u;
	record.words[7] = 0x7fu;
	/* complete, aligned, valid, magnitudes valid, full range */
	record.words[8] = 0x3eu;
	record.words[9] = 0x1000u;
	record.words[11] = 0x18700u;
	const auto first_order = static_cast<std::uint8_t>(
		chunk * msap1::harmonic_aggregate_orders_per_record + 1);
	const auto count = static_cast<std::uint8_t>(std::min<std::size_t>(
		msap1::harmonic_aggregate_orders_per_record,
		msap1::harmonic_max_order - first_order + 1));
	record.words[13] = channel | (static_cast<std::uint32_t>(chunk) << 3) |
		(static_cast<std::uint32_t>(first_order) << 7) |
		(static_cast<std::uint32_t>(count) << 15) |
		(static_cast<std::uint32_t>(
			 msap1::harmonic_chunks_per_channel) << 20) |
		(static_cast<std::uint32_t>(msap1::harmonic_max_order) << 24);
	record.words[14] = 1u | (15u << 2) | (1u << 30);
	record.words[15] = 127u | (50u << 8) | (10u << 16) | (3u << 24);
	for (std::size_t index = 0; index < count; ++index) {
		const auto order = first_order + index;
		const std::uint64_t packed =
			(static_cast<std::uint64_t>(channel) * 1000000u + order) |
			(std::uint64_t{1} << 60);
		record.words[16 + index * 2] = static_cast<std::uint32_t>(packed);
		record.words[17 + index * 2] =
			static_cast<std::uint32_t>(packed >> 32);
	}
	record.words[62] = 100u;
	record.words[63] = 114u;
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

} // namespace

int main()
{
	const auto first = make_record(7, 0, 0);
	require(first.header_valid(), "HARMONIC-v1 header is recognized");
	const auto decoded = msap1::decode_harmonic_record(first);
	require(decoded.channel == 0 && decoded.chunk == 0 &&
			decoded.first_order == 1 && decoded.order_count == 24,
		"chunk header decode");
	require(decoded.qualified_max_order == 127 &&
			decoded.nominal_frequency_hz == 50 &&
			decoded.cycle_count == 10 && decoded.filter_profile_id == 3,
		"metadata decode");
	require(decoded.entries[0].order == 1 &&
			decoded.entries[0].magnitude_micro_units == 1 &&
			decoded.entries[0].angle_millidegrees == 1000 &&
			decoded.entries[0].magnitude_valid &&
			decoded.entries[0].angle_valid,
		"packed entry decode");

	{
		auto bad = first;
		bad.words[17] |= 0x80000000u;
		rejects([&] { (void)msap1::decode_harmonic_record(bad); },
			"reserved entry bits must be rejected");
	}
	{
		auto bad = make_record(7, 0, 5);
		bad.words[30] = 1u; /* first padding entry after the seven orders */
		rejects([&] { (void)msap1::decode_harmonic_record(bad); },
			"nonzero padding must be rejected");
	}

	std::vector<msap1::HarmonicRecordChunk> chunks;
	for (std::uint8_t channel = 0; channel < msap1::harmonic_channel_count;
	     ++channel)
		for (std::uint8_t chunk = 0;
		     chunk < msap1::harmonic_chunks_per_channel; ++chunk)
			chunks.push_back(msap1::decode_harmonic_record(
				make_record(7, channel, chunk)));
	std::reverse(chunks.begin(), chunks.end());
	msap1::HarmonicFamilyAssembler assembler;
	std::optional<msap1::HarmonicSpectrumSnapshot> completed;
	for (const auto &chunk : chunks) {
		const auto update = assembler.accept(chunk);
		require(update.incomplete_families == 0,
			"one complete family has no loss");
		if (update.completed)
			completed = update.completed;
	}
	require(completed.has_value(), "all 42 unique chunks complete the family");
	if (completed) {
		require(completed->sequence == 7 && completed->full_range(),
			"completed family provenance");
		require(completed->channels[6][126].order == 127 &&
				completed->channels[6][126].magnitude_micro_units ==
					6000127u &&
				completed->channels[6][126].angle_millidegrees ==
					127000u,
			"last channel and order survive assembly");
	}
	rejects([&] { (void)assembler.accept(chunks.front()); },
		"a completed sequence cannot be replayed");
	const auto after_gap = assembler.accept(msap1::decode_harmonic_record(
		make_record(9, 0, 0)));
	require(after_gap.incomplete_families == 1,
		"a gap after a completed family is counted once");
	const auto replaced_after_gap = assembler.accept(
		msap1::decode_harmonic_record(make_record(11, 0, 0)));
	require(replaced_after_gap.incomplete_families == 2,
		"replacing a partial does not recount an earlier reported gap");

	/* Replacing an unfinished sequence 20 with 22 accounts for both the
	 * abandoned family 20 and wholly missing family 21. */
	assembler.reset();
	(void)assembler.accept(msap1::decode_harmonic_record(
		make_record(20, 0, 0)));
	const auto skipped = assembler.accept(msap1::decode_harmonic_record(
		make_record(22, 0, 0)));
	require(skipped.incomplete_families == 2,
		"abandoned and wholly skipped families are counted");
	{
		auto mismatch = make_record(22, 0, 1);
		mismatch.words[14] = 49999u;
		rejects([&] {
			(void)assembler.accept(
				msap1::decode_harmonic_record(mismatch));
		}, "cross-chunk provenance mismatch must be rejected");
	}

	/* R5C1 aggregate families use 23 magnitudes per chunk and reserve the
	 * final two words for the exact base-family sequence range. */
	assembler.reset();
	std::optional<msap1::HarmonicSpectrumSnapshot> aggregate;
	for (std::uint8_t channel = 0; channel < msap1::harmonic_channel_count;
	     ++channel)
		for (std::uint8_t chunk = 0;
		     chunk < msap1::harmonic_chunks_per_channel; ++chunk) {
			const auto decoded_aggregate = msap1::decode_harmonic_record(
				make_aggregate_record(3, channel, chunk));
			const auto update = assembler.accept(decoded_aggregate);
			if (update.completed)
				aggregate = update.completed;
		}
	require(aggregate && aggregate->period ==
			mnc::meter::MeasurementPeriod::Cycles150_180 &&
			aggregate->contributors == 15 && aggregate->aligned &&
			!aggregate->contaminated && aggregate->interval_valid() &&
			aggregate->first_source_sequence == 100 &&
			aggregate->last_source_sequence == 114,
		"aggregate harmonic provenance survives atomic assembly");
	if (aggregate)
		require(aggregate->channels[6][126].magnitude_micro_units ==
				6000127u &&
				!aggregate->channels[6][126].angle_valid,
			"aggregate magnitude-only order 127 survives assembly");

	if (failures != 0) {
		std::fprintf(stderr, "FAILED: %d check(s)\n", failures);
		return EXIT_FAILURE;
	}
	std::printf("PASS: harmonic_decode_test\n");
	return EXIT_SUCCESS;
}
