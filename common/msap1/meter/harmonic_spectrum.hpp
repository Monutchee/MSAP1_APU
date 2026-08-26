#pragma once

/**
 * @file harmonic_spectrum.hpp
 * @brief M16 HARMONIC-v1 chunk decode and atomic spectrum-family assembly.
 */

#include "msap1/meter/meter_record.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace msap1 {

inline constexpr std::size_t harmonic_channel_count = 7;
inline constexpr std::size_t harmonic_max_order = 127;
inline constexpr std::size_t harmonic_orders_per_record = 24;
inline constexpr std::size_t harmonic_chunks_per_channel = 6;
inline constexpr std::size_t harmonic_records_per_family =
	harmonic_channel_count * harmonic_chunks_per_channel;

/** One IEC-style harmonic subgroup and its central-line phase. */
struct HarmonicPoint {
	std::uint8_t order = 0;
	std::uint64_t magnitude_micro_units = 0;
	std::uint32_t angle_millidegrees = 0;
	bool magnitude_valid = false;
	bool angle_valid = false;
};

/** One validated 256-byte HARMONIC-v1 record projected to typed fields. */
struct HarmonicRecordChunk {
	std::uint32_t sequence = 0;
	std::uint32_t configuration_generation = 0;
	std::uint32_t sample_rate_hz = 0;
	std::uint32_t sample_count = 0;
	std::uint8_t valid_mask = 0;
	std::uint32_t status = 0;
	std::uint64_t first_sample = 0;
	std::uint32_t emit_drops = 0;
	std::uint32_t result_drops = 0;
	std::uint8_t channel = 0;
	std::uint8_t chunk = 0;
	std::uint8_t first_order = 0;
	std::uint8_t order_count = 0;
	std::uint8_t chunk_count = 0;
	std::uint8_t max_order = 0;
	std::uint32_t measured_frequency_millihz = 0;
	std::uint8_t qualified_max_order = 0;
	std::uint8_t nominal_frequency_hz = 0;
	std::uint8_t cycle_count = 0;
	std::uint8_t filter_profile_id = 0;
	std::array<HarmonicPoint, harmonic_orders_per_record> entries{};
};

/** A complete family. No partially assembled spectrum is externally visible. */
struct HarmonicSpectrumSnapshot {
	std::uint32_t sequence = 0;
	std::uint32_t configuration_generation = 0;
	std::uint32_t sample_rate_hz = 0;
	std::uint32_t sample_count = 0;
	std::uint8_t valid_mask = 0;
	std::uint32_t status = 0;
	std::uint64_t first_sample = 0;
	std::uint32_t emit_drops = 0;
	std::uint32_t result_drops = 0;
	std::uint32_t measured_frequency_millihz = 0;
	std::uint8_t qualified_max_order = 0;
	std::uint8_t nominal_frequency_hz = 0;
	std::uint8_t cycle_count = 0;
	std::uint8_t filter_profile_id = 0;
	std::array<std::array<HarmonicPoint, harmonic_max_order>,
		   harmonic_channel_count> channels{};

	[[nodiscard]] bool arithmetic_error() const { return (status & 0x1u) != 0; }
	[[nodiscard]] bool grid_locked() const { return (status & 0x4u) != 0; }
	[[nodiscard]] bool conditioner_valid() const { return (status & 0x8u) != 0; }
	[[nodiscard]] bool fft_valid() const { return (status & 0x10u) != 0; }
	[[nodiscard]] bool full_range() const { return (status & 0x20u) != 0; }
	[[nodiscard]] bool first_after_discontinuity() const
	{
		return (status & 0x40u) != 0;
	}
	[[nodiscard]] bool rate_limited() const { return (status & 0x80u) != 0; }
};

/** Decode and fully validate one HARMONIC-v1 chunk. */
[[nodiscard]] HarmonicRecordChunk decode_harmonic_record(
	const MeterRecord &record);

struct HarmonicAssemblyUpdate {
	/* Families abandoned or wholly skipped before this accepted chunk. */
	std::uint32_t incomplete_families = 0;
	std::optional<HarmonicSpectrumSnapshot> completed{};
};

/**
 * Stateful, bounded assembler for the one producer sequence space.
 *
 * Chunks may arrive in any order, but duplicates, stale sequences, and any
 * cross-chunk provenance mismatch are rejected. Completion is atomic at all
 * 42 unique chunks. The object retains no spectrum history.
 */
class HarmonicFamilyAssembler {
public:
	[[nodiscard]] HarmonicAssemblyUpdate accept(
		const HarmonicRecordChunk &chunk);
	void reset();

private:
	struct PartialFamily {
		HarmonicSpectrumSnapshot snapshot{};
		std::array<bool, harmonic_records_per_family> received{};
		std::size_t received_count = 0;
	};

	std::optional<PartialFamily> partial_{};
	std::optional<std::uint32_t> last_completed_sequence_{};
};

} // namespace msap1
