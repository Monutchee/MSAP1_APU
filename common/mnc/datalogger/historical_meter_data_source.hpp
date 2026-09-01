#pragma once

#include "mnc/datalogger/types.hpp"

#include <span>

namespace mnc::datalogger {

/** Product adapter for a typed, durable historical meter-data authority. */
class HistoricalMeterDataSource {
public:
	virtual ~HistoricalMeterDataSource() = default;

	/** Return every selected point in the half-open UTC window [start,end). */
	[[nodiscard]] virtual std::vector<HistoricalSample> query(
		mnc::meter::MeasurementPeriod period,
		std::span<const mnc::meter::MeterAttributeKey> attributes,
		UtcWindow window) const = 0;

	/** Expected source-point coverage for one output bucket and attribute. */
	[[nodiscard]] virtual std::uint64_t expected_sample_count(
		mnc::meter::MeasurementPeriod period,
		mnc::meter::MeterAttributeKey attribute,
		UtcWindow window) const = 0;
};

} // namespace mnc::datalogger
