#pragma once

#include "mnc/datalogger/historical_meter_data_source.hpp"

namespace mnc::datalogger {

/** Reusable product-facing generation interface. */
class Datalogger {
public:
	virtual ~Datalogger() = default;

	[[nodiscard]] virtual GeneratedDataset generate(
		const DatalogJobSnapshot &job, UtcWindow completed_window,
		UtcNanoseconds generated_at) const = 0;
};

/**
 * Product-neutral aggregation engine shared by concrete product Dataloggers.
 * The injected source owns historian transport, paging and retention policy.
 */
class HistoricalDatasetGenerator final {
public:
	explicit HistoricalDatasetGenerator(
		const HistoricalMeterDataSource &source) noexcept : source_(source) {}

	[[nodiscard]] GeneratedDataset generate(const DatalogJobSnapshot &job,
		UtcWindow completed_window, UtcNanoseconds generated_at) const;

private:
	const HistoricalMeterDataSource &source_;
};

} // namespace mnc::datalogger
