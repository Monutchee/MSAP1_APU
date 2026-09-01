#pragma once

#include "mnc/MeterDataProvider/attributes/meter_attribute_set.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace mnc::meter {

/**
 * Quality of the UTC mapping attached to a measurement.
 *
 * This is deliberately separate from ReadingQuality: a perfectly valid
 * electrical sample may be measured while the device clock is in holdover or
 * has not yet been synchronized.
 */
enum class TimeQuality : std::uint8_t {
	Unsynchronized = 0,
	Synchronized,
	Holdover,
};

/**
 * Measurement-time provenance supplied by the ingestion pipeline.
 *
 * The values describe when the source block was measured, not when a client
 * requested a snapshot. UTC fields are absent when no trusted mapping was
 * available; sample-domain fields remain useful in that case.
 */
struct MeterSnapshotTiming {
	TimeQuality quality = TimeQuality::Unsynchronized;
	std::optional<std::int64_t> utc_start_nanoseconds;
	std::optional<std::uint64_t> utc_uncertainty_nanoseconds;
	std::optional<std::uint64_t> first_sample_index;
	std::optional<std::uint32_t> sample_count;
	std::optional<std::uint32_t> sample_rate_hz;
	/* Keep this wider than the current product wire field: generic consumers
	 * should not inherit an MSAP1-specific limit when longer windows appear. */
	std::optional<std::uint32_t> cycle_count;
	std::optional<std::uint32_t> nominal_frequency_hz;
	/* Aggregate provenance. These fields are absent for a Basic block. For
	 * live partial intervals, expected_end_sample_index identifies the
	 * programmed normative boundary while sample_count describes only the
	 * samples accumulated so far. */
	std::optional<std::uint32_t> source_interval_count;
	std::optional<std::uint64_t> first_source_sequence;
	std::optional<std::uint64_t> last_source_sequence;
	std::optional<std::uint64_t> expected_end_sample_index;
	std::optional<std::uint32_t> overshoot_samples;
	std::optional<bool> time_aligned;
	std::optional<bool> contaminated;
	std::optional<bool> boundary_valid;
};

struct MeterAttributeValue {
	MeterAttributeKey attribute;
	MeterUnit unit = MeterUnit::MicroVolts;
	ReadingQuality quality = ReadingQuality::Unavailable;
	std::int64_t value = 0;
	std::uint64_t source_sequence = 0;
	std::int64_t measured_at_nanoseconds = 0;
	std::uint32_t sample_count = 0;
	std::int64_t calculation_window_nanoseconds = 0;
};

struct EnergySnapshotMetadata {
	std::uint64_t session_id = 0;
	std::uint64_t reset_epoch = 0;
	std::uint64_t last_sample_index = 0;
	std::uint64_t accepted_samples = 0;
	std::uint64_t skipped_samples = 0;
	std::uint32_t accepted_blocks = 0;
	std::uint32_t skipped_blocks = 0;
	bool saturated = false;
	bool incomplete_input = false;
	bool discontinuity = false;
};

struct DemandSnapshotMetadata {
	std::uint64_t session_id = 0;
	std::uint64_t peak_reset_epoch = 0;
	std::uint64_t last_sample_index = 0;
	std::uint64_t interval_anchor_sample = 0;
	std::uint32_t source_interval_count = 0;
	std::uint32_t source_status = 0;
	std::uint32_t window_seconds = 0;
	std::uint32_t update_seconds = 0;
	std::uint32_t profile_generation = 0;
	std::uint8_t method = 0;
	std::array<std::uint64_t, 4> import_peak_samples{};
	std::array<std::uint64_t, 4> export_peak_samples{};
	bool time_aligned = false;
	bool contaminated = false;
	bool boundary_valid = false;
	bool saturated = false;
	bool incomplete_input = false;
};

/** Exact audit provenance of an IEC 61000-4-30 ten-second frequency result. */
struct Frequency10sSnapshotMetadata {
	std::uint64_t interval_end_sample_index = 0;
	std::uint64_t utc_start_nanoseconds = 0;
	std::uint64_t utc_end_nanoseconds = 0;
	std::uint64_t utc_uncertainty_nanoseconds = 0;
	std::uint32_t measured_sample_rate_millihz = 0;
	std::uint32_t source_sequence = 0;
	std::uint32_t boundary_generation = 0;
	std::uint32_t source_status = 0;
	std::uint32_t status = 0;
	std::uint32_t reasons = 0;
	std::uint32_t observer_drop_count = 0;
	std::uint8_t guard_flags = 0;
	std::uint32_t observed_crossings = 0;
	std::uint32_t included_crossings = 0;
	std::uint32_t rejected_cycles = 0;
	std::uint64_t duration_q16_samples = 0;
	std::int64_t first_crossing_q16_samples = 0;
	std::int64_t last_crossing_q16_samples = 0;
	std::uint8_t nominal_frequency_hz = 0;
	std::uint8_t reference_channel = 0;
	std::uint8_t filter_profile = 0;
	std::uint8_t calibration_profile = 0;
};

struct MeterSnapshot {
	MeasurementPeriod period = MeasurementPeriod::Basic;
	std::uint64_t sequence = 0;
	std::uint32_t configuration_generation = 0;
	std::int64_t updated_at_nanoseconds = 0;
	std::optional<MeterSnapshotTiming> timing;
	std::optional<EnergySnapshotMetadata> energy;
	std::optional<DemandSnapshotMetadata> demand;
	std::optional<Frequency10sSnapshotMetadata> frequency_10s;
	std::vector<MeterAttributeValue> values;
};

struct MeterSnapshotRequest {
	MeasurementPeriod period = MeasurementPeriod::Basic;
	/**
	 * Explicit selections retain request order after deduplication.  An empty
	 * selection expands to the provider's canonical capability order.
	 * Known-but-unsupported keys are returned with Unavailable quality;
	 * malformed or unknown identities are rejected.
	 */
	std::vector<MeterAttributeKey> attributes;
};

/**
 * One measurement period that the provider actually supports and its
 * canonical attribute order.  Providers do not advertise vocabulary-only
 * future periods with empty attribute lists.
 */
struct MeterCapabilities {
	MeasurementPeriod period = MeasurementPeriod::Basic;
	std::vector<MeterAttributeKey> attributes;
};

} // namespace mnc::meter
