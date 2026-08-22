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
/* BASIC-v4 (metrology M7): the 10/12-cycle merge tier that retired the
 * MTR1 engine. Interior identical to MTR1-v3 for the envelope, timing
 * word, per-lane slots, and words 56..63; additions: block last-sample
 * anchor (words 14/15), merged line-line RMS (words 51..53, micro-units,
 * 32-bit), and status bit 2 = first block after a discontinuity. */
inline constexpr std::uint32_t meter_periodic_format = 0x00010004u;
/* POWER v1 (metrology M8): emitted by the 10/12-cycle tier on the same
 * stream immediately after each BASIC-v4 record, same sequence and
 * anchors. Per phase P (s64 pW), S (u64 pVA), true PF (s32 millionths,
 * 0 = undefined when S is 0); arithmetic totals; per-lane crest factors
 * (u32 ten-thousandths, 0 when RMS is 0). */
inline constexpr std::uint32_t meter_power_format = 0x00070001u;
/* PHASOR v2 (metrology M9; angle convention finalized with M11): third
 * record of each 10/12-cycle block, same sequence and anchors.
 * Fundamental (synchronous-correlation) quantities: per lane fundamental
 * RMS (u32 micro-units) + angle (u32 millidegrees in the industry
 * [0, 360000) convention, RELATIVE TO Va which reads exactly 0); VLL
 * phasors (complex differences); per phase the V-I displacement angle,
 * Q1 (s64 picovars, lagging/inductive positive), P1 (s64 picowatts),
 * displacement PF (s32 millionths, 0 = undefined when S1 is 0), and a
 * 2-bit load-nature code; arithmetic totals. Word 51 packs the natures
 * and bit 8 = angle-reference-valid. Status bit 1 = at least one merged
 * cycle had no usable frequency reference (every phasor word suspect). */
inline constexpr std::uint32_t meter_phasor_format = 0x00080002u;
/* UNBALANCE v1 (metrology M10): fourth record of each 10/12-cycle block,
 * same sequence and anchors. Symmetrical components of the fundamental
 * phasors: zero/positive/negative sequence RMS (u32 micro-units) + angle
 * (u32 millidegrees, [0, 360000), relative to Va) for voltage (16..21) and
 * current (22..27, IA/IB/IC never IN); |X0|/|X1| and UNBL = |X2|/|X1| in
 * millionths at words 28..31 (0 + flags-word validity bit clear =
 * undefined when |X1| = 0, clamped at the u32 rail). Word 32 flags: bit
 * 0 V ratios valid, bit 1 I ratios valid, bit 8 angle-reference valid.
 * Status bit 1 mirrors the PHASOR record (frequency-reference loss). */
inline constexpr std::uint32_t meter_unbalance_format = 0x00090002u;
/* AGG v3 (metrology M11): the 150/180-cycle tier record from
 * Agg150_180CycleEngine (Mtr2Engine retired). MTR2-v2 interior plus:
 * words 36/37 = interval last-sample index, words 38..40 = VAB/VBC/VCA
 * aggregate RMS (u32 micro-units). SEMANTIC upgrade: per-lane RMS is the
 * whole-interval finalize of the summed raw accumulators (mean-corrected
 * under the committed dc_remove, sample-weighted), no longer sqrt(mean
 * of 15 block-RMS squares). */
inline constexpr std::uint32_t meter_aggregate_format = 0x00020003u;
/* AGG-POWER/PHASOR/UNBAL v1 (M11): the aggregate tier's siblings, same
 * sequence/anchors as their AGG-v3 record; payload word maps (16+)
 * IDENTICAL to the basic-period POWER/PHASOR/UNBAL v1 maps. Word 13
 * carries the MTR2 shape word and 14/15 the folded basic-sequence range. */
inline constexpr std::uint32_t meter_aggregate_power_format = 0x00100001u;
inline constexpr std::uint32_t meter_aggregate_phasor_format = 0x00110002u;
inline constexpr std::uint32_t meter_aggregate_unbalance_format = 0x00120002u;
/* TEN-MINUTE v1 (M13): UTC-boundary-aligned aggregation of complete basic
 * blocks. The fundamental record carries the boundary target, actual close,
 * overshoot, and contamination state. Its POWER/PHASOR/UNBAL siblings retain
 * the corresponding aggregate payload maps. */
inline constexpr std::uint32_t meter_ten_minute_format = 0x000C0001u;
inline constexpr std::uint32_t meter_ten_minute_power_format = 0x00130001u;
inline constexpr std::uint32_t meter_ten_minute_phasor_format = 0x00140002u;
inline constexpr std::uint32_t meter_ten_minute_unbalance_format = 0x00150002u;
/* TWO-HOUR v1 (M14): twelve consecutive, complete ten-minute intervals
 * merged from their accumulator images.  The fundamental record reuses the
 * M13 boundary/composition layout; words 14/15 identify the contributing
 * ten-minute sequence range.  Sibling payload maps remain identical to the
 * other aggregate tiers. */
inline constexpr std::uint32_t meter_two_hour_format = 0x000D0001u;
inline constexpr std::uint32_t meter_two_hour_power_format = 0x00160001u;
inline constexpr std::uint32_t meter_two_hour_phasor_format = 0x00170002u;
inline constexpr std::uint32_t meter_two_hour_unbalance_format = 0x00180002u;
/* M15 live-partial views. These records expose an open accumulator for
 * operations only. They use independent sequence spaces and can never replace
 * the immutable completed M13/M14 results. */
inline constexpr std::uint32_t meter_ten_minute_open_format = 0x000E0001u;
inline constexpr std::uint32_t meter_ten_minute_open_power_format = 0x00190001u;
inline constexpr std::uint32_t meter_ten_minute_open_phasor_format = 0x001A0002u;
inline constexpr std::uint32_t meter_ten_minute_open_unbalance_format = 0x001B0002u;
inline constexpr std::uint32_t meter_two_hour_open_format = 0x000F0001u;
inline constexpr std::uint32_t meter_two_hour_open_power_format = 0x001C0001u;
inline constexpr std::uint32_t meter_two_hour_open_phasor_format = 0x001D0002u;
inline constexpr std::uint32_t meter_two_hour_open_unbalance_format = 0x001E0002u;
/* PQEVT v1 (metrology M12): the sliding Urms(1/2) tier's record, on its
 * OWN producer port with its own sequence space. Word 13 selects the
 * kind (0 periodic heartbeat, 1 event start, 2 event end) and carries the
 * event type (0 none, 1 sag, 2 swell, 3 interruption), the affected phase
 * mask, and the locked/fallback/armed flags. Words 16..27: latest
 * Urms(1/2), the span's min and max (micro-volts), and Irms(1/2)
 * (micro-amperes), all per phase A/B/C. Word 28 ties an event START to
 * its END; 29/30 the event duration in samples; 31 the half-cycle update
 * count. Words 32..36 echo the reference and thresholds the record was
 * evaluated against, so a stored event stays interpretable. */
inline constexpr std::uint32_t meter_pq_event_format = 0x000B0001u;
/* Single-cycle diagnostic records (PL metrology roadmap M2). */
inline constexpr std::uint32_t meter_single_cycle_format = 0x000A0005u;

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

/** Decoded TEN-MINUTE record word 8: interval-level status flags. */
struct MeterTenMinuteStatus {
	bool arithmetic_error = false;
	bool complete = false;
	bool time_aligned = false;
	bool contaminated = false;
	bool boundary_valid = false;
	bool open_interval = false;
	bool non_normative = false;
};

/** Decoded TEN-MINUTE word 13 plus word 41 composition. */
struct MeterTenMinuteComposition {
	std::uint16_t basic_block_count = 0;
	std::uint8_t nominal_frequency_hz = 0;
	std::uint8_t flags = 0;
	std::uint32_t cycle_count = 0;
};

/* M14 deliberately reuses the merge-safe M13 interval envelope.  Keep
 * distinct semantic names at the API boundary even though the wire fields
 * have the same representation. */
using MeterTwoHourStatus = MeterTenMinuteStatus;
using MeterTwoHourComposition = MeterTenMinuteComposition;

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
		        record_format() == meter_power_format ||
			record_format() == meter_phasor_format ||
			record_format() == meter_unbalance_format ||
			record_format() == meter_aggregate_format ||
				record_format() == meter_aggregate_power_format ||
				record_format() == meter_aggregate_phasor_format ||
				record_format() == meter_aggregate_unbalance_format ||
				record_format() == meter_ten_minute_format ||
				record_format() == meter_ten_minute_power_format ||
				record_format() == meter_ten_minute_phasor_format ||
				record_format() == meter_ten_minute_unbalance_format ||
				record_format() == meter_two_hour_format ||
				record_format() == meter_two_hour_power_format ||
				record_format() == meter_two_hour_phasor_format ||
				record_format() == meter_two_hour_unbalance_format ||
				record_format() == meter_ten_minute_open_format ||
				record_format() == meter_ten_minute_open_power_format ||
				record_format() == meter_ten_minute_open_phasor_format ||
				record_format() == meter_ten_minute_open_unbalance_format ||
				record_format() == meter_two_hour_open_format ||
				record_format() == meter_two_hour_open_power_format ||
				record_format() == meter_two_hour_open_phasor_format ||
				record_format() == meter_two_hour_open_unbalance_format ||
			record_format() == meter_pq_event_format ||
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

	/* ---- periodic (BASIC-v4, 0x00010004) fields ----------------------- */

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

	/* ---- clock-aligned ten-minute aggregate (M13) -------------------- */

	MeterTenMinuteStatus ten_minute_status() const
	{
		const auto status_word = word(8);
		return {
			(status_word & (1u << 0)) != 0u,
			(status_word & (1u << 1)) != 0u,
			(status_word & (1u << 2)) != 0u,
			(status_word & (1u << 3)) != 0u,
			(status_word & (1u << 4)) != 0u,
			(status_word & (1u << 5)) != 0u,
			(status_word & (1u << 6)) != 0u,
		};
	}

	MeterTenMinuteComposition ten_minute_composition() const
	{
		const auto shape = word(13);
		return {
			static_cast<std::uint16_t>(shape & 0xffffu),
			static_cast<std::uint8_t>((shape >> 16) & 0xffu),
			static_cast<std::uint8_t>((shape >> 24) & 0xffu),
			word(41),
		};
	}

	std::uint64_t ten_minute_actual_last_sample_index() const
	{
		return unsigned64(36);
	}
	std::uint64_t ten_minute_target_sample_index() const
	{
		return unsigned64(42);
	}
	std::uint32_t ten_minute_overshoot_samples() const { return word(44); }

	/* ---- two-hour aggregate (M14) ------------------------------------
	 * The shared AggregationEngine intentionally keeps the M13 layout so
	 * downstream decoders can share validation without duplicating a wire
	 * contract.  In this tier, words 14/15 are TEN-MINUTE sequences. */
	MeterTwoHourStatus two_hour_status() const
	{
		return ten_minute_status();
	}
	MeterTwoHourComposition two_hour_composition() const
	{
		return ten_minute_composition();
	}
	std::uint64_t two_hour_actual_last_sample_index() const
	{
		return ten_minute_actual_last_sample_index();
	}
	std::uint64_t two_hour_target_sample_index() const
	{
		return ten_minute_target_sample_index();
	}
	std::uint32_t two_hour_overshoot_samples() const
	{
		return ten_minute_overshoot_samples();
	}
};

static_assert(sizeof(MeterRecord) == meter_record_size,
	      "meter record must match the fixed PL DMA format");
static_assert(std::is_trivially_copyable_v<MeterRecord>);

} // namespace msap1

#endif // MSAP1_METER_RECORD_HPP
