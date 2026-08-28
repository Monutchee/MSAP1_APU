#pragma once

#include "mnc/MeterDataProvider/attributes/meter_attribute.hpp"
#include "mnc/MeterDataProvider/stream/database_policy.hpp"
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

/**
 * Can the volatile projections be rebuilt from everything ever published?
 * @p oldest_cursor and @p session_start_cursor come from the stream's
 * status; @p persisted_high_water from the historian's own persistent
 * projections (its durable proof that history predates this spool session).
 *
 * Two ways coverage is lost: records pruned within the current spool session
 * (the oldest retained cursor moved past the session's first possible one),
 * and a spool session that began only after history already existed — which
 * is how a volatile spool restart looks, and how an EMPTY spool
 * (oldest_cursor of 0) is kept truthful instead of passing as complete.
 * The replaced heuristic, oldest_cursor > 1, did exactly that.
 */
[[nodiscard]] constexpr bool backfill_is_incomplete(std::uint64_t oldest_cursor,
	std::uint64_t session_start_cursor, std::uint64_t persisted_high_water)
{
	if (oldest_cursor > session_start_cursor + 1)
		return true;
	return persisted_high_water > 0 &&
		session_start_cursor > persisted_high_water;
}

/**
 * A replay is required only when a dataset is newly routed to volatile
 * memory. Retention-only changes and moves to persistent storage preserve an
 * already materialized target and must not trigger a full spool scan.
 */
[[nodiscard]] constexpr bool historian_policy_transition_requires_backfill(
	std::span<const mnc::meter_stream::DatabaseStoragePolicy> current,
	std::span<const mnc::meter_stream::DatabaseStoragePolicy> candidate)
{
	using mnc::meter_stream::StorageBackend;
	for (const auto &next : candidate) {
		if (next.backend != StorageBackend::memory)
			continue;
		bool already_memory = false;
		for (const auto &prior : current) {
			if (prior.dataset == next.dataset) {
				already_memory = prior.backend == StorageBackend::memory;
				break;
			}
		}
		if (!already_memory)
			return true;
	}
	return false;
}

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
	/** Stage one validated aggregate-harmonic chunk and atomically materialize
	 * the family when all 42 fragments are durable. Base families remain
	 * latest-only and are rejected here. */
	[[nodiscard]] bool append_harmonic_record(const MeterRecord &record,
		std::uint64_t stream_cursor,
		std::int64_t measured_at_nanoseconds);
	[[nodiscard]] std::vector<HistoryPoint> query(const HistoryQuery &query) const;
	[[nodiscard]] HistorianStatus status() const;
	/** Highest stream cursor the persistent projections have committed, or 0
	 * when they hold nothing.  This is the historian's own durable coverage
	 * mark, independent of the spool's (possibly volatile) consumer state. */
	[[nodiscard]] std::uint64_t persisted_stream_high_water() const;
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
