#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace mnc::meter_stream {

/** Measurement-time provenance retained beside the producer's exact record. */
struct RecordTimingProvenance {
	std::uint64_t first_sample_index = 0;
	std::uint32_t sample_count = 0;
	std::uint32_t cycle_count = 0;
	std::uint8_t time_quality = 0;
	std::optional<std::int64_t> utc_start_nanoseconds;
	std::optional<std::uint64_t> utc_uncertainty_nanoseconds;
};

/** Product-neutral durable envelope around one exact producer record. */
struct MeterStreamRecord {
	std::uint64_t cursor = 0;
	std::uint32_t record_format = 0;
	std::uint16_t record_kind = 0;
	std::uint8_t measurement_period = 0;
	std::uint64_t source_sequence = 0;
	/* Distinguishes records which deliberately share one producer sequence
	 * and sample span (for example the 42 chunks of one M16 family). */
	std::uint16_t source_fragment = 0;
	std::uint32_t configuration_generation = 0;
	std::int64_t ingested_at_nanoseconds = 0;
	RecordTimingProvenance timing{};
	std::vector<std::byte> payload;
};

} // namespace mnc::meter_stream
