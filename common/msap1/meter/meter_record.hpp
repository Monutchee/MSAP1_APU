#ifndef MSAP1_METER_RECORD_HPP
#define MSAP1_METER_RECORD_HPP

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <type_traits>

namespace msap1 {

/* The PL record wire formats are normative in the PL repository:
 * MSAP1_PL/SourceData/HLS_DesignFile/common/include/measurement_record.hpp
 * (both producers are HLS engines that build and serialize their own
 * records). This header mirrors that contract; PL and APU change in the
 * same release — the kernel framing is positional, so there is no version
 * negotiation on the wire.
 *
 * Every record shares one envelope in words 0..12 (magic, format, size,
 * per-producer sequence, generation, sample rate, sample count, valid
 * mask, status, 64-bit first-sample timestamp, transport drop words), so
 * provenance, continuity, and transport health decode identically for
 * every record type; only words 13+ are format-specific. */

inline constexpr std::size_t meter_channel_count = 8;
inline constexpr std::size_t meter_record_word_count = 64;
inline constexpr std::size_t meter_record_size = 256;
inline constexpr std::uint32_t meter_record_magic = 0x3152544du;
/* Record type rides in [31:16] of word 1, version in [15:0]. The
 * reservation table (energy/demand/harmonics/PQ) lives with the PL
 * contract header; allocate there, never ad hoc. */
inline constexpr std::uint32_t meter_periodic_format = 0x00010003u;
inline constexpr std::uint32_t meter_aggregate_format = 0x00020002u;
/* Single-cycle diagnostic records (PL metrology roadmap M2). */
inline constexpr std::uint32_t meter_single_cycle_format = 0x000A0002u;

struct MeterChannelReading {
	bool valid = false;
	std::int64_t mean_micro_units = 0;
	std::uint32_t rms_count = 0;
	std::int64_t rms_micro_units = 0;
};

/** Decoded periodic record word 13: PL cycle-timing provenance. */
struct MeterTimingWord {
	/* Configured nominal frequency in Hz (50 or 60) — configuration
	 * echoed by the PL, never a measurement. */
	std::uint8_t nominal_frequency_hz = 0;
	/* Complete cycles closed in this block (10 or 12 when locked). */
	std::uint8_t cycle_count = 0;
	bool cycle_locked = false;
	bool free_run_fallback = false;
	bool first_block_after_apply = false;
};

/** Decoded aggregate record word 8: record-level status flags. */
struct MeterAggregateStatus {
	/* Set when any internal aggregate computation overflowed. */
	bool arithmetic_error = false;
	/* Always set on emitted records: the PL emits only complete
	 * 15-block aggregates, never partial ones. */
	bool complete = false;
	/* Set only when all 15 basic frequency readings were valid. */
	bool frequency_valid = false;
};

/** Decoded aggregate record word 13: aggregation composition. */
struct MeterAggregateComposition {
	/* Contributing basic blocks (15 on every emitted record). */
	std::uint8_t basic_block_count = 0;
	/* Configured nominal frequency in Hz (50 or 60) — configuration
	 * echoed by the PL, never a measurement. */
	std::uint8_t nominal_frequency_hz = 0;
	/* Total complete cycles across the contributing blocks: 150 at a
	 * 50 Hz nominal, 180 at 60 Hz. */
	std::uint16_t cycle_count = 0;
};

struct MeterFrequencyReading {
	bool enabled = false;
	bool valid = false;
	bool reference_valid = false;
	bool out_of_range = false;
	bool timed_out = false;
	bool arithmetic_error = false;
	std::uint32_t millihz = 0;
	std::uint32_t period_q16_samples = 0;
	std::uint32_t measurement_sequence = 0;
	std::uint8_t mode = 0;
	std::uint8_t reference_channel = 0;
	std::uint8_t cycles_used = 0;
};

struct MeterRecord {
	std::array<std::uint32_t, meter_record_word_count> words{};

	std::uint32_t word(std::size_t index) const
	{
		if (index >= words.size())
			throw std::out_of_range("meter record word index");
		if constexpr (std::endian::native == std::endian::little)
			return words[index];
		return std::byteswap(words[index]);
	}

	std::uint64_t unsigned64(std::size_t low_word) const
	{
		return static_cast<std::uint64_t>(word(low_word)) |
		       (static_cast<std::uint64_t>(word(low_word + 1)) << 32);
	}

	std::int64_t signed64(std::size_t low_word) const
	{
		return std::bit_cast<std::int64_t>(unsigned64(low_word));
	}

	/* ---- common envelope (words 0..12, every record type) ----------- */

	std::uint32_t record_format() const { return word(1); }

	bool header_valid() const
	{
		return word(0) == meter_record_magic &&
		       (record_format() == meter_periodic_format ||
			record_format() == meter_aggregate_format ||
			record_format() == meter_single_cycle_format) &&
		       word(2) == meter_record_size;
	}

	/* Per-producer monotone sequence; the periodic and aggregate streams
	 * count independently (the aggregate stream starts at 1 and is never
	 * continuous with the basic stream). */
	std::uint32_t sequence() const { return word(3); }
	std::uint32_t configuration_generation() const { return word(4); }
	std::uint32_t sample_rate_hz() const { return word(5); }
	/* Word 6: the ACTUAL sample count this record covers — one basic
	 * block (varies with grid frequency when cycle timing is locked) or
	 * the sum over an aggregate's 15 contributing blocks. */
	std::uint32_t block_sample_count() const { return word(6); }
	std::uint8_t valid_mask() const { return static_cast<std::uint8_t>(word(7)); }
	std::uint32_t status() const { return word(8); }

	/* Words 9/10: first sample of this record's interval on the PL
	 * 64-bit free-running conversion-domain counter (never reset by
	 * configuration or UTC). The last sample is intentionally not in the
	 * record: last = first + block_sample_count() - 1. */
	std::uint64_t first_sample_index() const { return unsigned64(9); }

	/* Words 11/12: transport health, carried in-record ("as of this
	 * record"). Both are constant 0 by construction in the current
	 * engines — emission is blocking and every closed window is
	 * finalized — so any nonzero value is a fault. */
	std::uint32_t emit_drops() const { return word(11); }
	std::uint32_t result_drops() const { return word(12); }

	/* ---- periodic (MTR1, 0x00010003) fields -------------------------- */

	MeterTimingWord timing() const
	{
		const auto timing_word = word(13);
		return {
			static_cast<std::uint8_t>(timing_word & 0xffu),
			static_cast<std::uint8_t>((timing_word >> 8) & 0xffu),
			(timing_word & (1u << 16)) != 0u,
			(timing_word & (1u << 17)) != 0u,
			(timing_word & (1u << 18)) != 0u,
		};
	}

	MeterChannelReading channel(std::size_t index) const
	{
		if (index >= meter_channel_count)
			throw std::out_of_range("meter channel index");
		const auto base = 16u + index * 5u;
		return {
			(valid_mask() & (1u << index)) != 0u,
			signed64(base),
			word(base + 2u),
			signed64(base + 3u),
		};
	}

	MeterFrequencyReading frequency() const
	{
		const auto frequency_status = word(57);
		return {
			(frequency_status & (1u << 0)) != 0u,
			(frequency_status & (1u << 1)) != 0u,
			(frequency_status & (1u << 2)) != 0u,
			(frequency_status & (1u << 5)) != 0u,
			(frequency_status & (1u << 6)) != 0u,
			(frequency_status & (1u << 7)) != 0u,
			word(56),
			word(58),
			word(59),
			static_cast<std::uint8_t>((frequency_status >> 8) & 0x7u),
			static_cast<std::uint8_t>((frequency_status >> 12) & 0xfu),
			static_cast<std::uint8_t>((frequency_status >> 16) & 0xffu),
		};
	}

	/* Words 60..63: capture diagnostics, latched at block close (they can
	 * therefore lag a concurrently running capture by the frames still in
	 * flight when the block closed). */
	std::uint32_t capture_frames() const { return word(60); }
	std::uint32_t header_errors() const { return word(61); }
	std::uint32_t fifo_overflows() const { return word(62); }
	std::uint32_t adc_alerts() const { return word(63); }

	/* ---- aggregate (MTR2, 0x00020002) fields ------------------------- */

	std::uint32_t aggregate_sequence() const { return sequence(); }
	std::uint32_t aggregate_sample_count() const
	{
		return block_sample_count();
	}
	/* Same conversion-domain counter as the periodic first-sample index:
	 * the first sample of the FIRST contributing basic block. */
	std::uint64_t aggregate_first_sample_index() const
	{
		return first_sample_index();
	}
	MeterAggregateStatus aggregate_status() const
	{
		const auto status_word = word(8);
		return {
			(status_word & (1u << 0)) != 0u,
			(status_word & (1u << 1)) != 0u,
			(status_word & (1u << 2)) != 0u,
		};
	}
	MeterAggregateComposition aggregate_composition() const
	{
		const auto composition_word = word(13);
		return {
			static_cast<std::uint8_t>(composition_word & 0xffu),
			static_cast<std::uint8_t>((composition_word >> 8) &
						  0xffu),
			static_cast<std::uint16_t>(composition_word >> 16),
		};
	}
	/* Words 14/15: BASIC-stream sequence range folded into this
	 * aggregate (inclusive), for cross-stream correlation. */
	std::uint32_t first_basic_sequence() const { return word(14); }
	std::uint32_t last_basic_sequence() const { return word(15); }
	/* Words 16..31: aggregate RMS in signed 64-bit micro-units, two
	 * words (lo, hi) per channel. Channel order is identical to MTR1:
	 * Ia, Ib, Ic, In, Vc, Vb, Va, ch7 = 0. Validity comes from the
	 * word-7 mask (the AND across the 15 contributing blocks). */
	std::int64_t aggregate_rms_micro_units(std::size_t index) const
	{
		if (index >= meter_channel_count)
			throw std::out_of_range("meter channel index");
		return signed64(16u + index * 2u);
	}
	/* Word 32: mean fundamental frequency in millihertz; 0 whenever
	 * aggregate_status().frequency_valid is clear. */
	std::uint32_t aggregate_frequency_millihz() const { return word(32); }
	/* Words 33..35: aggregation-engine diagnostics as of this emit (the
	 * counters that back the AGG_* PL registers). */
	std::uint32_t aggregate_reset_count() const { return word(33); }
	std::uint32_t aggregate_ineligible_count() const { return word(34); }
	std::uint32_t aggregate_continuity_count() const { return word(35); }
};

static_assert(sizeof(MeterRecord) == meter_record_size,
	      "meter record must match the fixed PL DMA format");
static_assert(std::is_trivially_copyable_v<MeterRecord>);

} // namespace msap1

#endif // MSAP1_METER_RECORD_HPP
