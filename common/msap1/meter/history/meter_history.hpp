#pragma once

#include "mnc/MeterDataProvider/attributes/meter_attribute.hpp"
#include "mnc/MeterDataStreamer/database_policy.hpp"
#include "msap1/meter/meter_data.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace msap1::history {

struct HistoryPoint {
	std::int64_t measured_at_nanoseconds = 0;
	std::uint64_t source_sequence = 0;
	mnc::meter::MeterAttributeId attribute =
		mnc::meter::MeterAttributeId::Frequency;
	std::int64_t value = 0;
	MeasurementQuality quality = MeasurementQuality::unavailable;
};

struct HistoryQuery {
	MeasurementPeriod period = MeasurementPeriod::Basic;
	std::vector<mnc::meter::MeterAttributeId> attributes;
	std::int64_t start_nanoseconds = 0;
	std::int64_t end_nanoseconds = 0;
	std::uint32_t limit = 10000;
};

/** Exact query surface implemented by the running historian. */
struct HistorianCapabilities {
	std::vector<MeasurementPeriod> periods;
	std::vector<mnc::meter::MeterAttributeId> attributes;
	std::uint32_t maximum_points = 50000;
};

struct HistorianStatus {
	bool healthy = true;
	bool migration_in_progress = false;
	bool backfill_incomplete = false;
	std::uint64_t acknowledged_cursor = 0;
	std::uint64_t oldest_available_stream_cursor = 0;
	std::uint64_t block_count = 0;
	std::uint64_t storage_bytes = 0;
	struct DatasetStatus {
		mnc::meter_stream::DatabaseDataset dataset =
			mnc::meter_stream::DatabaseDataset::basic;
		mnc::meter_stream::StorageBackend backend =
			mnc::meter_stream::StorageBackend::persistent;
		std::uint64_t block_count = 0;
		std::uint64_t storage_bytes = 0;
		std::optional<std::int64_t> oldest_nanoseconds;
		std::optional<std::int64_t> newest_nanoseconds;
	};
	std::vector<DatasetStatus> datasets;
};

/** Routes each typed PL result to the configured volatile/persistent store. */
class MeterHistoryStore final {
public:
	MeterHistoryStore(std::filesystem::path persistent_path,
		std::vector<mnc::meter_stream::DatabaseStoragePolicy> policies);
	~MeterHistoryStore();
	MeterHistoryStore(const MeterHistoryStore &) = delete;
	MeterHistoryStore &operator=(const MeterHistoryStore &) = delete;

	void append(const MeterUpdate &update, std::uint64_t stream_cursor,
		std::int64_t measured_at_nanoseconds);
	[[nodiscard]] std::vector<HistoryPoint> query(const HistoryQuery &query) const;
	[[nodiscard]] HistorianStatus status() const;
	[[nodiscard]] std::vector<mnc::meter_stream::DatabaseStoragePolicy>
	policies() const;
	/**
	 * Prepare newly selected volatile targets for a complete spool replay.
	 * Persistent targets are deliberately preserved and merged idempotently.
	 */
	void prepare_policy_migration(
		const std::vector<mnc::meter_stream::DatabaseStoragePolicy> &policies);
	void apply_policies(std::vector<mnc::meter_stream::DatabaseStoragePolicy> policies);
	/**
	 * Remove the selected historical projections from both volatile and
	 * persistent storage.  The replay floor prevents retained spool records
	 * at or below @p through_stream_cursor from resurrecting deleted data.
	 */
	void clear_datasets(
		std::span<const mnc::meter_stream::DatabaseDataset> datasets,
		std::uint64_t through_stream_cursor);
	/**
	 * Replace the complete historian database with a fresh schema while
	 * preserving storage policy and the live stream acknowledgement point.
	 */
	void recreate_database(std::uint64_t through_stream_cursor);

private:
	class Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace msap1::history
