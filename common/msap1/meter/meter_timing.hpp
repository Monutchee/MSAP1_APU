#ifndef MSAP1_METER_TIMING_HPP
#define MSAP1_METER_TIMING_HPP

/**
 * Class A measurement timing vocabulary.
 *
 * The basic measurement unit is a cycle-defined block, not a time interval:
 * 10 complete grid cycles at a 50 Hz nominal, 12 at 60 Hz. The nominal
 * duration is about 200 ms, but 200 ms is never the semantic definition —
 * the actual duration varies with grid frequency, intentionally. See
 * docs/System_Architecture/TIMING_MODEL.md for the full model.
 */

#include "mnc/MeterDataProvider/attributes/meter_attribute.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>

namespace msap1::meter {

using SystemTime = std::chrono::system_clock::time_point;

/**
 * Configured nominal grid frequency. This is configuration, NOT the measured
 * frequency: it selects the cycles-per-block rule and the free-run fallback
 * window and must never be inferred from a measurement.
 */
enum class NominalFrequency : std::uint8_t {
	Hz50 = 50,
	Hz60 = 60,
};

/**
 * Aggregation tier of a decoded meter update. Basic, 150/180-cycle,
 * clock-aligned ten-minute, and two-hour tiers are produced today.
 */
using MeasurementPeriod = mnc::meter::MeasurementPeriod;

/**
 * UTC synchronization state of the measurement timebase. Time quality is a
 * property of the UTC mapping only — it never invalidates the electrical
 * measurement itself (MeasurementQuality stays independent).
 */
enum class TimeQuality : std::uint8_t {
	Unsynchronized = 0,
	Synchronized,
	Holdover,
};

/** Cycles per basic measurement block: 50 Hz -> 10 cycles, 60 Hz -> 12. */
[[nodiscard]] constexpr std::uint32_t
cycles_per_basic_block(NominalFrequency nominal)
{
	switch (nominal) {
	case NominalFrequency::Hz50:
		return 10u;
	case NominalFrequency::Hz60:
		return 12u;
	}
	throw std::invalid_argument("invalid nominal frequency");
}

/**
 * Basic blocks folded into one 150/180-cycle aggregate. The first Class A
 * aggregation tier is defined as exactly 15 consecutive eligible basic
 * blocks — cycle-defined like the basic block itself, never a 3-second
 * timer.
 */
inline constexpr std::uint32_t basic_blocks_per_aggregate = 15u;

/** Cycles per 150/180-cycle aggregate: 50 Hz -> 150, 60 Hz -> 180. */
[[nodiscard]] constexpr std::uint32_t
cycles_per_aggregate(NominalFrequency nominal)
{
	return basic_blocks_per_aggregate * cycles_per_basic_block(nominal);
}

/**
 * Timing identity of one decoded basic measurement block.
 *
 * The PL contributes the block boundaries, cycle-lock provenance flags, and
 * the free-running conversion-domain sample index; the APU stamps the UTC
 * mapping and its quality at decode time.
 */
struct BlockTiming {
	/* Wire sequence is uint32, stored widened. No wraparound extension is
	 * performed yet; the value is the raw wire sequence. */
	std::uint64_t sequence = 0;
	std::uint32_t configuration_generation = 0;
	/* Index of the block's first sample on the PL 64-bit free-running
	 * conversion counter. Never reset by configuration or UTC changes;
	 * last sample = first_sample_index + sample_count - 1. */
	std::uint64_t first_sample_index = 0;
	/* Actual samples in this block — varies with grid frequency when
	 * cycle-locked; equals the fallback window in free-run. */
	std::uint32_t sample_count = 0;
	std::uint32_t sample_rate_hz = 0;
	/* Complete cycles closed in this block (10 or 12 when locked). */
	std::uint16_t cycle_count = 0;
	NominalFrequency nominal_frequency = NominalFrequency::Hz60;
	bool cycle_locked = false;
	bool free_run_fallback = false;
	bool first_block_after_apply = false;
	bool utc_resynchronized = false;
	TimeQuality time_quality = TimeQuality::Unsynchronized;
	/* UTC of the first sample via the measurement timebase mapping;
	 * absent while unsynchronized. */
	std::optional<SystemTime> utc_start;
	/* Error bound on utc_start, from the sync point the label came from.
	 * Present exactly when utc_start is present. */
	std::optional<std::uint64_t> utc_uncertainty_ns;
};

/**
 * Whether a block may enter Class A aggregation (150/180-cycle, 10-min…).
 *
 * Only complete cycle-locked basic blocks aggregate: free-run fallback and
 * partial blocks would silently dilute a cycle-defined interval with
 * time-defined data. first_block_after_apply is conservatively ineligible —
 * the current PL RTL cannot assert it together with cycle_locked anyway, so
 * this term is defense in depth against a future RTL that could.
 *
 * Eligibility is an aggregation concern only: it must never feed back into
 * MeasurementQuality, which describes the electrical measurement itself.
 */
[[nodiscard]] constexpr bool
class_a_aggregation_eligible(const BlockTiming &timing)
{
	return timing.cycle_locked && !timing.free_run_fallback &&
	       !timing.first_block_after_apply &&
	       timing.cycle_count ==
		       cycles_per_basic_block(timing.nominal_frequency);
}

/**
 * Timing identity of one decoded aggregate record.
 *
 * R5C1 is the authoritative aggregator: it folds exactly 15 consecutive
 * ELIGIBLE basic blocks (same generation and nominal; sample-range continuous
 * except for the one marked UTC transition) into one aggregate and emits only
 * complete aggregates. The APU never recomputes any of that; it decodes the
 * record and, exactly as for BlockTiming, stamps the UTC mapping and its
 * quality at decode time.
 */
struct AggregateTiming {
	/* Wire sequence is uint32, stored widened. Aggregates count on their
	 * OWN sequence stream (starting at 1), independent of the basic
	 * block sequence stream. */
	std::uint64_t sequence = 0;
	std::uint32_t configuration_generation = 0;
	/* First sample of the first contributing basic block on the PL
	 * 64-bit free-running conversion counter — the same domain as
	 * BlockTiming::first_sample_index. */
	std::uint64_t first_sample_index = 0;
	/* Actual last contributing sample. Normally this is first+count-1; an
	 * intentional UTC-overlap aggregate has a shorter physical span. */
	std::uint64_t last_sample_index = 0;
	/* Total samples across all contributing basic blocks. */
	std::uint32_t sample_count = 0;
	std::uint32_t sample_rate_hz = 0;
	/* Source-tier sequence range folded into this aggregate (inclusive).
	 * This is the BASIC stream for 150/180-cycle and ten-minute records,
	 * and the TEN-MINUTE stream for the two-hour record.  The legacy field
	 * names are retained to avoid churn in existing consumers. */
	std::uint32_t first_basic_sequence = 0;
	std::uint32_t last_basic_sequence = 0;
	/* Contributing source intervals: 15 basic blocks for 150/180-cycle,
	 * variable basic blocks for ten-minute, and 12 ten-minute intervals for
	 * two-hour. */
	std::uint16_t basic_block_count = 0;
	/* Total complete cycles in this interval. */
	std::uint32_t cycle_count = 0;
	NominalFrequency nominal_frequency = NominalFrequency::Hz60;
	bool arithmetic_error = false;
	/* Mean-frequency validity: set only when all 15 basic frequency
	 * readings were valid (informative — the standardized frequency
	 * interval is the 10 s tier, not this one). */
	bool frequency_valid = false;
	bool utc_overlap = false;
	bool utc_resynchronized = false;
	/* M13/M14 boundary provenance. These remain false/absent for the
	 * 150/180-cycle tier. A contaminated interval is still retained for
	 * diagnostics, but its electrical readings decode as invalid. */
	bool time_aligned = false;
	bool contaminated = false;
	bool boundary_valid = false;
	std::optional<std::uint64_t> target_sample_index;
	std::optional<std::uint32_t> overshoot_samples;
	TimeQuality time_quality = TimeQuality::Unsynchronized;
	/* UTC of the first sample via the measurement timebase mapping;
	 * absent while unsynchronized. */
	std::optional<SystemTime> utc_start;
	/* Error bound on utc_start, from the sync point the label came from.
	 * Present exactly when utc_start is present. */
	std::optional<std::uint64_t> utc_uncertainty_ns;
};

} // namespace msap1::meter

#endif // MSAP1_METER_TIMING_HPP
