#pragma once

#include "mnc/MeterDataProvider/attributes/meter_attribute_set.hpp"

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
	/* Keep this wider than the current product wire field: generic consumers
	 * should not inherit an MSAP1-specific limit when longer windows appear. */
	std::optional<std::uint32_t> cycle_count;
	std::optional<std::uint32_t> nominal_frequency_hz;
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

struct MeterSnapshot {
	MeasurementPeriod period = MeasurementPeriod::Basic;
	std::uint64_t sequence = 0;
	std::uint32_t configuration_generation = 0;
	std::int64_t updated_at_nanoseconds = 0;
	std::optional<MeterSnapshotTiming> timing;
	std::vector<MeterAttributeValue> values;
};

struct MeterSnapshotRequest {
	MeasurementPeriod period = MeasurementPeriod::Basic;
	std::vector<MeterAttributeKey> attributes;
};

struct MeterCapabilities {
	MeasurementPeriod period = MeasurementPeriod::Basic;
	std::vector<MeterAttributeKey> attributes;
};

} // namespace mnc::meter
