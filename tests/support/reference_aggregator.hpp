#ifndef MSAP1_TESTS_REFERENCE_AGGREGATOR_HPP
#define MSAP1_TESTS_REFERENCE_AGGREGATOR_HPP

/**
 * NON-AUTHORITATIVE software reference for 150/180-cycle aggregation.
 *
 * R5C1 is the authoritative aggregator: it folds 15 consecutive eligible
 * basic blocks into one 150/180-cycle aggregate record, and the APU only
 * DECODES that record.
 * Production code must not include this header — it exists solely so tests
 * can verify the decoded wire quantities against an independent implementation of
 * the pinned arithmetic. Everything here is integer math (unsigned 128-bit
 * accumulation, integer floor square root, floor divisions).
 *
 * Pinned semantics (IEC 61000-4-30 aggregation):
 *
 *   RMS        X_agg = floor(sqrt(floor(sum(X_i^2) / 15))) computed in the
 *              PL Q16.16 internal domain, then converted to the wire's
 *              integer micro-units (floor, i.e. >> 16). Unweighted: every
 *              basic interval contributes equally even though the actual
 *              sample counts vary slightly with grid frequency.
 *
 *   Frequency  floor(sum(f_i) / 15) millihertz, valid only when all 15
 *              basic readings were valid; 0 when invalid. Informative only:
 *              the standardized frequency interval is the 10 s tier, which
 *              is out of scope for this record.
 */

#include "msap1/meter/meter_record.hpp"
#include "msap1/meter/meter_timing.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace msap1::testing {

/* Q16.16 squares outgrow 64 bits (230 V is already ~2^44 in Q16.16
 * microvolts), so the sum of 15 squares accumulates in 128 bits. __int128
 * is a GCC/Clang extension; __extension__ keeps -Wpedantic quiet. Inputs
 * must stay below 2^62 so 15 squares cannot overflow the accumulator —
 * far beyond any physical RMS value. */
__extension__ typedef unsigned __int128 Uint128;

/** One basic block's contribution, in the PL Q16.16 internal domain. */
struct ReferenceBasicInput {
	/* Per-channel RMS in Q16.16 micro-units, PL channel order
	 * (Ia, Ib, Ic, In, Vc, Vb, Va, ch7 = 0). */
	std::array<std::uint64_t, meter_channel_count> rms_q16{};
	std::uint32_t frequency_millihz = 0;
	bool frequency_valid = false;
};

/** Expected aggregate payload computed from the 15 basic inputs. */
struct ReferenceAggregate {
	/* Aggregate RMS in the Q16.16 internal domain... */
	std::array<std::uint64_t, meter_channel_count> rms_q16{};
	/* ...and converted to the wire's integer micro-units. The wire
	 * field is signed 64-bit only for layout symmetry with the Basic record; the
	 * arithmetic is unsigned throughout and never goes negative. */
	std::array<std::int64_t, meter_channel_count> rms_micro_units{};
	/* Mean frequency in millihertz; 0 unless frequency_valid. */
	std::uint32_t frequency_millihz = 0;
	/* Set only when all 15 basic frequency readings were valid. */
	bool frequency_valid = false;
};

/** Largest u with u*u <= value (classic bitwise integer square root). */
[[nodiscard]] constexpr std::uint64_t floor_sqrt(Uint128 value)
{
	Uint128 result = 0;
	Uint128 bit = Uint128{1} << 126;
	while (bit > value)
		bit >>= 2;
	while (bit != 0) {
		if (value >= result + bit) {
			value -= result + bit;
			result = (result >> 1) + bit;
		} else {
			result >>= 1;
		}
		bit >>= 2;
	}
	/* sqrt of a 128-bit value always fits 64 bits. */
	return static_cast<std::uint64_t>(result);
}

/** Compute the expected aggregate exactly as the PL is pinned to. */
[[nodiscard]] constexpr ReferenceAggregate
reference_aggregate(const std::array<ReferenceBasicInput,
				     meter::basic_blocks_per_aggregate> &blocks)
{
	ReferenceAggregate aggregate{};
	for (std::size_t channel = 0; channel != meter_channel_count;
	     ++channel) {
		/* Square root of the arithmetic mean of the squares; both
		 * the division and the root take the floor. */
		Uint128 sum_of_squares = 0;
		for (const auto &block : blocks) {
			const Uint128 value = block.rms_q16[channel];
			sum_of_squares += value * value;
		}
		aggregate.rms_q16[channel] = floor_sqrt(
			sum_of_squares / meter::basic_blocks_per_aggregate);
		/* Q16.16 -> integer micro-units, floor. */
		aggregate.rms_micro_units[channel] = static_cast<std::int64_t>(
			aggregate.rms_q16[channel] >> 16);
	}
	std::uint64_t frequency_sum = 0;
	bool all_valid = true;
	for (const auto &block : blocks) {
		frequency_sum += block.frequency_millihz;
		all_valid = all_valid && block.frequency_valid;
	}
	aggregate.frequency_valid = all_valid;
	aggregate.frequency_millihz =
		all_valid ? static_cast<std::uint32_t>(
				    frequency_sum /
				    meter::basic_blocks_per_aggregate)
			  : 0u;
	return aggregate;
}

} // namespace msap1::testing

#endif // MSAP1_TESTS_REFERENCE_AGGREGATOR_HPP
