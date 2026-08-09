#pragma once

#include "mnc/MeterDataProvider/attributes/meter_attribute_set.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace mnc::meter {

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
