#ifndef MSAP1_METER_TIMING_HPP
#define MSAP1_METER_TIMING_HPP

/**
 * Class A measurement timing vocabulary.
 *
 * The basic measurement unit is a cycle-defined block, not a time interval:
 * 10 complete grid cycles at a 50 Hz nominal, 12 at 60 Hz. The nominal
 * duration is about 200 ms, but 200 ms is never the semantic definition —
 * the actual duration varies with grid frequency, intentionally. See
 * docs/TIMING_MODEL.md for the full model.
 */

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
 * Aggregation tier of a decoded meter update. Only Basic is produced today;
 * the longer tiers are reserved for future aggregate record formats.
 */
enum class MeasurementPeriod : std::uint8_t {
	Basic = 0,
	Cycles150_180,
	Min10,
	Hour2,
};

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
	/* Complete cycles closed in this block (10 or 12 when locked). */
	std::uint16_t cycle_count = 0;
	NominalFrequency nominal_frequency = NominalFrequency::Hz60;
	bool cycle_locked = false;
	bool free_run_fallback = false;
	bool first_block_after_apply = false;
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

} // namespace msap1::meter

#endif // MSAP1_METER_TIMING_HPP
