#include "msap1/meter/history/meter_history.hpp"

#include "mnc/storage/sqlite/sqlite_database.hpp"
#include "msap1/meter/harmonic_spectrum.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace msap1::history {
namespace {

using mnc::meter::MeterAttributeId;
using mnc::storage::sqlite::Database;
using mnc::storage::sqlite::Transaction;

std::int64_t period_value(MeasurementPeriod period)
{
	return static_cast<std::int64_t>(period);
}

MeasurementPeriod period_for(mnc::meter_stream::DatabaseDataset dataset)
{
	switch (dataset) {
	case mnc::meter_stream::DatabaseDataset::basic:
		return MeasurementPeriod::Basic;
	case mnc::meter_stream::DatabaseDataset::cycles_150_180:
		return MeasurementPeriod::Cycles150_180;
	case mnc::meter_stream::DatabaseDataset::minutes_10:
		return MeasurementPeriod::Min10;
	case mnc::meter_stream::DatabaseDataset::hours_2:
		return MeasurementPeriod::Hour2;
	case mnc::meter_stream::DatabaseDataset::raw_record_spool:
	case mnc::meter_stream::DatabaseDataset::harmonic_cycles_150_180:
	case mnc::meter_stream::DatabaseDataset::harmonic_minutes_10:
	case mnc::meter_stream::DatabaseDataset::harmonic_hours_2:
		throw std::invalid_argument("spool is not a historian dataset");
	}
	throw std::invalid_argument("unknown historian dataset");
}

bool harmonic_dataset(mnc::meter_stream::DatabaseDataset dataset)
{
	using Dataset = mnc::meter_stream::DatabaseDataset;
	return dataset == Dataset::harmonic_cycles_150_180 ||
		dataset == Dataset::harmonic_minutes_10 ||
		dataset == Dataset::harmonic_hours_2;
}

mnc::meter_stream::DatabaseDataset harmonic_dataset_for(
	MeasurementPeriod period)
{
	using Dataset = mnc::meter_stream::DatabaseDataset;
	switch (period) {
	case MeasurementPeriod::Cycles150_180:
		return Dataset::harmonic_cycles_150_180;
	case MeasurementPeriod::Min10:
		return Dataset::harmonic_minutes_10;
	case MeasurementPeriod::Hour2:
		return Dataset::harmonic_hours_2;
	case MeasurementPeriod::Basic:
	case MeasurementPeriod::Min10Live:
	case MeasurementPeriod::Hour2Live:
		break;
	}
	throw std::invalid_argument("unsupported harmonic historian period");
}

MeasurementPeriod harmonic_period_for(
	mnc::meter_stream::DatabaseDataset dataset)
{
	using Dataset = mnc::meter_stream::DatabaseDataset;
	switch (dataset) {
	case Dataset::harmonic_cycles_150_180:
		return MeasurementPeriod::Cycles150_180;
	case Dataset::harmonic_minutes_10: return MeasurementPeriod::Min10;
	case Dataset::harmonic_hours_2: return MeasurementPeriod::Hour2;
	default:
		throw std::invalid_argument("dataset is not harmonic history");
	}
}

mnc::meter_stream::DatabaseDataset dataset_for(MeasurementPeriod period)
{
	switch (period) {
	case MeasurementPeriod::Basic:
		return mnc::meter_stream::DatabaseDataset::basic;
	case MeasurementPeriod::Cycles150_180:
		return mnc::meter_stream::DatabaseDataset::cycles_150_180;
	case MeasurementPeriod::Min10:
		return mnc::meter_stream::DatabaseDataset::minutes_10;
	case MeasurementPeriod::Hour2:
		return mnc::meter_stream::DatabaseDataset::hours_2;
	case MeasurementPeriod::Min10Live:
	case MeasurementPeriod::Hour2Live:
		throw std::invalid_argument(
			"non-normative open intervals are not historian datasets");
	}
	throw std::invalid_argument("unknown historian measurement period");
}

void validate_historian_policies(
	const std::vector<mnc::meter_stream::DatabaseStoragePolicy> &policies)
{
	if (policies.size() != 7)
		throw std::invalid_argument(
			"historian requires exactly seven dataset policies");
	std::set<mnc::meter_stream::DatabaseDataset> datasets;
	for (const auto &policy : policies) {
		mnc::meter_stream::validate_database_policy(policy);
		if (policy.dataset ==
		    mnc::meter_stream::DatabaseDataset::raw_record_spool)
			throw std::invalid_argument(
				"spool policy cannot be applied to the historian");
		if (!datasets.insert(policy.dataset).second)
			throw std::invalid_argument("duplicate historian dataset policy");
	}
}

bool supported_attribute(MeterAttributeId attribute)
{
	switch (attribute) {
	case MeterAttributeId::Frequency:
	case MeterAttributeId::VanRms:
	case MeterAttributeId::VbnRms:
	case MeterAttributeId::VcnRms:
	case MeterAttributeId::IaRms:
	case MeterAttributeId::IbRms:
	case MeterAttributeId::IcRms:
	case MeterAttributeId::InRms:
		return true;
	case MeterAttributeId::VabRms:
	case MeterAttributeId::VbcRms:
	case MeterAttributeId::VcaRms:
		return false;
	}
	return false;
}

std::uint64_t logical_period_bytes(Database &database, MeasurementPeriod period)
{
	auto usage = database.prepare(R"SQL(
SELECT COALESCE(SUM(96 +
 (SELECT COUNT(*) * 40 FROM measurement_values v WHERE v.block_id=b.id)),0)
FROM measurement_blocks b WHERE b.period=?
)SQL");
	usage.bind(1, period_value(period));
	(void)usage.step();
	return static_cast<std::uint64_t>(usage.integer(0));
}

std::uint64_t logical_harmonic_bytes(Database &database,
	MeasurementPeriod period)
{
	auto usage = database.prepare(R"SQL(
SELECT COALESCE(SUM(160 + LENGTH(payload)),0)
FROM harmonic_families WHERE period=?
)SQL");
	usage.bind(1, period_value(period));
	(void)usage.step();
	return static_cast<std::uint64_t>(usage.integer(0));
}

struct DatasetRange {
	std::uint64_t count = 0;
	std::optional<std::int64_t> oldest;
	std::optional<std::int64_t> newest;
};

DatasetRange dataset_range(Database &database, MeasurementPeriod period,
	bool harmonic)
{
	const std::string table = harmonic
		? "harmonic_families"
		: "measurement_blocks";
	DatasetRange result;
	auto count = database.prepare(
		"SELECT COUNT(*) FROM " + table + " WHERE period=?");
	count.bind(1, period_value(period));
	(void)count.step();
	result.count = static_cast<std::uint64_t>(count.integer(0));
	if (result.count == 0)
		return result;

	/*
	 * MIN()/MAX() combined with COUNT() scans every matching index entry.
	 * Keep the exact count, but obtain each endpoint with an ordered LIMIT so
	 * the (period, measured_at_ns) covering index can seek directly to it.
	 * This matters for forever-retained aggregate history: on the target an
	 * 825k-row combined range scan took 640 ms while both seeks took <10 ms.
	 */
	auto boundary = [&](std::string_view direction) {
		auto statement = database.prepare("SELECT measured_at_ns FROM " +
			table + " WHERE period=? ORDER BY measured_at_ns " +
			std::string(direction) + " LIMIT 1");
		statement.bind(1, period_value(period));
		if (!statement.step())
			throw std::runtime_error(
				"historian range changed while status lock was held");
		return statement.integer(0);
	};
	result.oldest = boundary("ASC");
	result.newest = boundary("DESC");
	return result;
}

std::uint64_t delete_oldest_harmonic_families(Database &database,
	MeasurementPeriod period, std::optional<std::int64_t> cutoff,
	std::uint64_t needed)
{
	const std::string sql = std::string(R"SQL(
SELECT id,160 + LENGTH(payload) FROM harmonic_families WHERE period=?
)SQL") + (cutoff ? " AND measured_at_ns<?" : "") +
		" ORDER BY measured_at_ns LIMIT 256";
	auto oldest = database.prepare(sql);
	oldest.bind(1, period_value(period));
	if (cutoff)
		oldest.bind(2, *cutoff);
	std::vector<std::pair<std::int64_t, std::uint64_t>> victims;
	while (oldest.step())
		victims.emplace_back(oldest.integer(0),
			static_cast<std::uint64_t>(oldest.integer(1)));
	if (victims.empty())
		return 0;
	auto remove = database.prepare("DELETE FROM harmonic_families WHERE id=?");
	std::uint64_t removed = 0;
	for (const auto &[id, bytes] : victims) {
		remove.bind(1, id);
		remove.execute();
		remove.reset();
		removed += bytes;
		if (removed >= needed)
			break;
	}
	return removed;
}

/*
 * Delete up to a bounded batch of the oldest blocks in @p period, restricted to
 * those strictly older than @p cutoff when one is given, and return the logical
 * bytes removed.
 *
 * Reading each block's own contribution as it goes is what lets the cached
 * period size stay exact without ever rescanning the table: a bare
 * DELETE ... WHERE measured_at_ns<? is cheaper per row but leaves the size
 * unknown, and recovering it means calling logical_period_bytes() again, which
 * is the O(total rows) cost the cache exists to remove.
 *
 * The correlated COUNT here is bounded by the batch, not by the table, and the
 * measurement_blocks_time index on (period, measured_at_ns) keeps the selection
 * proportional to the rows actually removed. Callers loop until this reports
 * zero, so one call can never stall on a large backlog.
 */
std::uint64_t delete_oldest_blocks(Database &database, MeasurementPeriod period,
	std::optional<std::int64_t> cutoff, std::uint64_t needed)
{
	const std::string sql = std::string(R"SQL(
SELECT b.id, 96 +
 (SELECT COUNT(*) * 40 FROM measurement_values v WHERE v.block_id=b.id)
FROM measurement_blocks b WHERE b.period=?)SQL") +
		(cutoff ? " AND b.measured_at_ns<?" : "") +
		" ORDER BY b.measured_at_ns LIMIT 256";
	auto oldest = database.prepare(sql);
	oldest.bind(1, period_value(period));
	if (cutoff)
		oldest.bind(2, *cutoff);
	std::vector<std::pair<std::int64_t, std::uint64_t>> victims;
	while (oldest.step())
		victims.emplace_back(oldest.integer(0),
			static_cast<std::uint64_t>(oldest.integer(1)));
	if (victims.empty())
		return 0;
	/* measurement_values has ON DELETE CASCADE with foreign_keys=ON, so
	 * removing the block removes its readings. */
	auto remove = database.prepare("DELETE FROM measurement_blocks WHERE id=?");
	std::uint64_t removed = 0;
	for (const auto &[id, bytes] : victims) {
		remove.bind(1, id);
		remove.execute();
		remove.reset();
		removed += bytes;
		/* Stop the moment enough has gone. The byte cap asks for exactly the
		 * surplus, so a batch selected for efficiency must not evict history
		 * the policy still allows -- deleting the whole batch would empty a
		 * projection whose surplus was a single block. */
		if (removed >= needed)
			break;
	}
	return removed;
}

std::uint64_t sqlite_family_size(const std::filesystem::path &path)
{
	std::uint64_t total = 0;
	for (const auto &candidate : {path, std::filesystem::path(path.string() + "-wal"),
		std::filesystem::path(path.string() + "-shm")}) {
		std::error_code error;
		const auto size = std::filesystem::file_size(candidate, error);
		if (!error)
			total += size;
	}
	return total;
}

template<typename Unit>
void add_reading(std::vector<std::pair<MeterAttributeId, const Reading<Unit> *>> &out,
	MeterAttributeId id, const Reading<Unit> &reading)
{
	out.emplace_back(id, &reading);
}

void initialize(Database &database, bool persistent)
{
	if (persistent) database.execute("PRAGMA journal_mode=WAL");
	database.execute("PRAGMA foreign_keys=ON");
	database.execute("PRAGMA synchronous=FULL");
	database.execute(R"SQL(
CREATE TABLE IF NOT EXISTS measurement_blocks(
 id INTEGER PRIMARY KEY,
 stream_cursor INTEGER NOT NULL UNIQUE,
 period INTEGER NOT NULL,
 record_kind INTEGER NOT NULL,
 source_sequence INTEGER NOT NULL,
 configuration_generation INTEGER NOT NULL,
 measured_at_ns INTEGER NOT NULL,
 window_start INTEGER NOT NULL,
 window_end INTEGER NOT NULL,
 quality INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS measurement_blocks_time
 ON measurement_blocks(period, measured_at_ns);
CREATE TABLE IF NOT EXISTS measurement_values(
 block_id INTEGER NOT NULL REFERENCES measurement_blocks(id) ON DELETE CASCADE,
 attribute_id INTEGER NOT NULL,
 signed_value INTEGER NOT NULL,
 quality INTEGER NOT NULL,
 source_sequence INTEGER NOT NULL,
 PRIMARY KEY(block_id, attribute_id)
);
CREATE TABLE IF NOT EXISTS historian_metadata(
 dataset INTEGER PRIMARY KEY,
 clear_through_cursor INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS harmonic_pending(
 period INTEGER NOT NULL,
 source_sequence INTEGER NOT NULL,
 source_fragment INTEGER NOT NULL,
 stream_cursor INTEGER NOT NULL UNIQUE,
 measured_at_ns INTEGER NOT NULL,
 payload BLOB NOT NULL,
 PRIMARY KEY(period,source_sequence,source_fragment)
);
CREATE TABLE IF NOT EXISTS harmonic_families(
 id INTEGER PRIMARY KEY,
 stream_cursor INTEGER NOT NULL UNIQUE,
 period INTEGER NOT NULL,
 source_sequence INTEGER NOT NULL,
 configuration_generation INTEGER NOT NULL,
 measured_at_ns INTEGER NOT NULL,
 first_sample INTEGER NOT NULL,
 sample_count INTEGER NOT NULL,
 target_sample INTEGER NOT NULL,
 contributors INTEGER NOT NULL,
 overshoot_samples INTEGER NOT NULL,
 status INTEGER NOT NULL,
 qualified_max_order INTEGER NOT NULL,
 first_source_sequence INTEGER NOT NULL,
 last_source_sequence INTEGER NOT NULL,
 payload BLOB NOT NULL,
 UNIQUE(period,configuration_generation,source_sequence)
);
CREATE INDEX IF NOT EXISTS harmonic_families_time
 ON harmonic_families(period, measured_at_ns);
)SQL");
}

void remove_database_family(const std::filesystem::path &path)
{
	for (const auto &candidate : {path,
		std::filesystem::path(path.string() + "-wal"),
		std::filesystem::path(path.string() + "-shm")}) {
		std::error_code error;
		const bool removed = std::filesystem::remove(candidate, error);
		if (error && !removed)
			throw std::runtime_error("remove historian database file " +
				candidate.string() + ": " + error.message());
	}
}

void remove_database_sidecars(const std::filesystem::path &path)
{
	for (const auto &candidate : {
		std::filesystem::path(path.string() + "-wal"),
		std::filesystem::path(path.string() + "-shm")}) {
		std::error_code error;
		(void)std::filesystem::remove(candidate, error);
		if (error)
			throw std::runtime_error("remove historian database sidecar " +
				candidate.string() + ": " + error.message());
	}
}

void write_clear_floor(Database &database,
	mnc::meter_stream::DatabaseDataset dataset, std::uint64_t cursor)
{
	auto statement = database.prepare(R"SQL(
INSERT INTO historian_metadata(dataset,clear_through_cursor) VALUES(?,?)
ON CONFLICT(dataset) DO UPDATE SET clear_through_cursor=excluded.clear_through_cursor
)SQL");
	statement.bind(1, static_cast<std::int32_t>(dataset));
	statement.bind(2, cursor);
	statement.execute();
}

} // namespace

class MeterHistoryStore::Impl final {
public:
	Impl(std::filesystem::path path,
		std::vector<mnc::meter_stream::DatabaseStoragePolicy> configured)
		: persistent_path(std::move(path)), memory(":memory:"), persistent(persistent_path),
		  manager(std::move(configured))
	{
		initialize(memory, false);
		initialize(persistent, true);
		auto floors = persistent.prepare(
			"SELECT dataset,clear_through_cursor FROM historian_metadata");
		while (floors.step()) {
			clear_floors[static_cast<mnc::meter_stream::DatabaseDataset>(
				floors.integer(0))] = static_cast<std::uint64_t>(
				floors.integer(1));
		}
		/* The stream owns the durable consumer cursor, but exposing the
		 * newest committed historian cursor immediately after restart keeps
		 * lag/status truthful before the next record arrives.  Volatile rows
		 * are rebuilt separately by MeterHistorianService. */
		auto latest = persistent.prepare(
			"SELECT MAX(value) FROM ("
			" SELECT COALESCE(MAX(stream_cursor),0) AS value FROM measurement_blocks"
			" UNION ALL SELECT COALESCE(MAX(stream_cursor),0) FROM harmonic_families"
			" UNION ALL SELECT COALESCE(MAX(stream_cursor),0) FROM harmonic_pending"
			")");
		(void)latest.step();
		acknowledged_cursor =
			static_cast<std::uint64_t>(latest.integer(0));
	}

	Database &database(MeasurementPeriod period)
	{
		const auto dataset = dataset_for(period);
		return database_for(dataset);
	}

	Database &database_for(mnc::meter_stream::DatabaseDataset dataset)
	{
		return manager.policy(dataset).backend ==
			mnc::meter_stream::StorageBackend::memory
			? memory : persistent;
	}

	std::filesystem::path persistent_path;
	Database memory;
	Database persistent;
	mnc::meter_stream::DatabasePolicyManager manager;
	mutable std::mutex mutex;
	std::uint64_t acknowledged_cursor = 0;
	std::map<mnc::meter_stream::DatabaseDataset, std::uint64_t> clear_floors;

	/*
	 * Cached logical size per period. logical_period_bytes() scans every block
	 * in the period with a correlated per-block COUNT, so calling it once per
	 * append made append() O(total rows): measured on hardware as a projection
	 * replay that ran at 11.4 records/s and decayed to 9.3 as the table grew.
	 * The size is seeded from that query once and then maintained
	 * incrementally, so every retention decision is O(1) plus the cost of the
	 * rows actually removed.
	 */
	std::map<MeasurementPeriod, std::uint64_t> logical_bytes;
	std::map<MeasurementPeriod, std::uint64_t> harmonic_logical_bytes;

	/* Seed on first use, then hand back the running total by reference so
	 * callers can adjust it in place. */
	std::uint64_t &tracked_bytes(Database &database, MeasurementPeriod period)
	{
		auto found = logical_bytes.find(period);
		if (found == logical_bytes.end())
			found = logical_bytes.emplace(
				period, logical_period_bytes(database, period)).first;
		return found->second;
	}

	std::uint64_t &tracked_harmonic_bytes(Database &database,
		MeasurementPeriod period)
	{
		auto found = harmonic_logical_bytes.find(period);
		if (found == harmonic_logical_bytes.end())
			found = harmonic_logical_bytes.emplace(period,
				logical_harmonic_bytes(database, period)).first;
		return found->second;
	}

	/*
	 * Drop the cache so it reseeds from the database. Required after any row
	 * change this class did not account for itself: dataset clearing, historian
	 * recreation, and backend migration all rewrite or discard whole tables.
	 * Reseeding is a scan, so it must stay off the per-append path.
	 */
	void forget_tracked_bytes()
	{
		logical_bytes.clear();
		harmonic_logical_bytes.clear();
	}
};

MeterHistoryStore::MeterHistoryStore(std::filesystem::path path,
	std::vector<mnc::meter_stream::DatabaseStoragePolicy> policies)
	: impl_([&] {
		validate_historian_policies(policies);
		return std::make_unique<Impl>(std::move(path), std::move(policies));
	}()) {}
MeterHistoryStore::~MeterHistoryStore() = default;

void MeterHistoryStore::append(const MeterUpdate &update,
	std::uint64_t stream_cursor, std::int64_t measured_at_ns)
{
	std::scoped_lock lock(impl_->mutex);
	const auto dataset = dataset_for(update.period);
	if (const auto floor = impl_->clear_floors.find(dataset);
	    floor != impl_->clear_floors.end() && stream_cursor <= floor->second) {
		impl_->acknowledged_cursor = std::max(
			impl_->acknowledged_cursor, stream_cursor);
		return;
	}
	auto &database = impl_->database(update.period);
	/*
	 * Seed the running size BEFORE this append's insert. Seeding is a scan, so
	 * doing it after the commit would count the row just written and then count
	 * it again below, leaving the total permanently one block high -- which
	 * makes the byte cap evict history the policy still allows.
	 */
	auto &tracked = impl_->tracked_bytes(database, update.period);
	Transaction transaction(database);
	auto block = database.prepare(R"SQL(
INSERT OR IGNORE INTO measurement_blocks(stream_cursor,period,record_kind,
 source_sequence,configuration_generation,measured_at_ns,window_start,window_end,quality)
VALUES(?,?,?,?,?,?,?,?,?)
)SQL");
	block.bind(1, stream_cursor); block.bind(2, period_value(update.period));
	block.bind(3, static_cast<std::int64_t>(update.kind));
	block.bind(4, update.sequence);
	block.bind(5, static_cast<std::int64_t>(update.configuration_generation));
	block.bind(6, measured_at_ns);
	std::uint64_t first = 0; std::uint64_t last = 0;
	if (update.timing) { first = update.timing->first_sample_index; last = first + update.timing->sample_count - 1u; }
	else if (update.aggregate_timing) { first = update.aggregate_timing->first_sample_index; last = update.aggregate_timing->last_sample_index; }
	block.bind(7, first); block.bind(8, last); block.bind(9, std::int32_t{1});
	block.execute();
	const bool inserted = database.changes() == 1;
	auto find = database.prepare("SELECT id FROM measurement_blocks WHERE stream_cursor=?");
	find.bind(1, stream_cursor); if (!find.step()) throw std::runtime_error("historian block missing");
	const auto block_id = find.integer(0);
	/*
	 * The statement must not stay in its row-available state across the
	 * COMMIT below: SQLite's WAL auto-checkpoint runs at commit and aborts
	 * whenever the committing connection still has an active statement.
	 * Leaving this SELECT unreset therefore blocked every checkpoint this
	 * store ever attempted, and the WAL grew without bound (1.4 GB on
	 * hardware) while the main database file stayed permanently empty.
	 */
	find.reset();
	/* A retained persistent projection can be encountered again while a
	 * volatile projection rebuilds. Its immutable stream cursor means the
	 * existing values are already authoritative; avoid needless writes/WAL
	 * growth on that idempotent replay path. */
	std::uint64_t appended_values = 0;
	if (inserted && update.fundamental) {
		const auto &f = *update.fundamental;
		auto value = database.prepare(R"SQL(
INSERT OR REPLACE INTO measurement_values(block_id,attribute_id,signed_value,quality,source_sequence)
VALUES(?,?,?,?,?)
)SQL");
		auto put = [&](MeterAttributeId id, const auto &reading) {
			value.bind(1, block_id); value.bind(2, static_cast<std::int32_t>(id));
			value.bind(3, reading.value); value.bind(4, static_cast<std::int32_t>(reading.quality));
			value.bind(5, reading.source_sequence); value.execute(); value.reset();
			/* Counted rather than assumed, so the size accounting below
			 * cannot drift if an attribute is added or removed here. */
			++appended_values;
		};
		put(MeterAttributeId::Frequency, f.frequency);
		put(MeterAttributeId::VanRms, f.voltage_ln.phase_a);
		put(MeterAttributeId::VbnRms, f.voltage_ln.phase_b);
		put(MeterAttributeId::VcnRms, f.voltage_ln.phase_c);
		put(MeterAttributeId::IaRms, f.current.phase_a);
		put(MeterAttributeId::IbRms, f.current.phase_b);
		put(MeterAttributeId::IcRms, f.current.phase_c);
		put(MeterAttributeId::InRms, f.current.neutral);
	}
	transaction.commit();
	impl_->acknowledged_cursor = std::max(impl_->acknowledged_cursor, stream_cursor);

	/* Retention is enforced only after the block is safely committed.  The
	 * stream spool remains the authoritative replay source, so pruning a
	 * volatile/basic projection cannot lose the accepted producer record. */
	const auto policy = impl_->manager.policy(dataset);
	/*
	 * The running size is seeded once per period and then maintained here, so
	 * neither cap costs a table scan. Only a genuine insert adds bytes: the
	 * INSERT OR IGNORE replay path leaves the row set untouched and writes no
	 * readings, so it must not adjust the total.
	 */
	if (inserted)
		tracked += 96u + appended_values * 40u;
	if (policy.retention.maximum_age) {
		const auto cutoff = measured_at_ns -
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				*policy.retention.maximum_age).count();
		/* Batched so a long-expired backlog drains across calls rather than
		 * stalling one append; at steady state this removes a single block. */
		for (;;) {
			/* Every expired block goes, so there is no byte target. */
			const auto removed = delete_oldest_blocks(database,
				update.period, cutoff,
				std::numeric_limits<std::uint64_t>::max());
			if (removed == 0)
				break;
			tracked -= std::min(tracked, removed);
		}
	}
	if (policy.retention.maximum_bytes) {
		while (tracked > *policy.retention.maximum_bytes) {
			/* Ask for exactly the surplus so nothing the policy still
			 * permits is evicted. */
			const auto removed = delete_oldest_blocks(database,
				update.period, std::nullopt,
				tracked - *policy.retention.maximum_bytes);
			if (removed == 0)
				break;
			tracked -= std::min(tracked, removed);
		}
	}
}

bool MeterHistoryStore::append_harmonic_record(const MeterRecord &record,
	std::uint64_t stream_cursor, std::int64_t measured_at_ns)
{
	const auto chunk = decode_harmonic_record(record);
	if (chunk.period == MeasurementPeriod::Basic)
		throw std::invalid_argument(
			"base harmonic families are latest-only");
	const auto dataset = harmonic_dataset_for(chunk.period);
	const auto fragment = static_cast<std::uint32_t>(chunk.channel) *
		harmonic_chunks_per_channel + chunk.chunk;

	std::scoped_lock lock(impl_->mutex);
	if (const auto floor = impl_->clear_floors.find(dataset);
	    floor != impl_->clear_floors.end() && stream_cursor <= floor->second) {
		impl_->acknowledged_cursor = std::max(
			impl_->acknowledged_cursor, stream_cursor);
		return false;
	}
	auto &database = impl_->database_for(dataset);
	auto &tracked = impl_->tracked_harmonic_bytes(database, chunk.period);
	Transaction transaction(database);

	/* The producer publishes one family contiguously. Bound abandoned staging
	 * state before inserting the current sequence so a missing fragment can
	 * never grow either backend without limit. */
	auto abandon = database.prepare(
		"DELETE FROM harmonic_pending WHERE period=? AND source_sequence<>?");
	abandon.bind(1, period_value(chunk.period));
	abandon.bind(2, static_cast<std::uint64_t>(chunk.sequence));
	abandon.execute();

	auto stage = database.prepare(R"SQL(
INSERT OR IGNORE INTO harmonic_pending(period,source_sequence,source_fragment,
 stream_cursor,measured_at_ns,payload) VALUES(?,?,?,?,?,?)
)SQL");
	stage.bind(1, period_value(chunk.period));
	stage.bind(2, static_cast<std::uint64_t>(chunk.sequence));
	stage.bind(3, static_cast<std::int32_t>(fragment));
	stage.bind(4, stream_cursor);
	stage.bind(5, measured_at_ns);
	stage.bind(6, std::as_bytes(std::span{&record, std::size_t{1}}));
	stage.execute();

	auto count = database.prepare(
		"SELECT COUNT(*) FROM harmonic_pending WHERE period=? AND source_sequence=?");
	count.bind(1, period_value(chunk.period));
	count.bind(2, static_cast<std::uint64_t>(chunk.sequence));
	(void)count.step();
	const auto complete = count.integer(0) ==
		static_cast<std::int64_t>(harmonic_records_per_family);
	count.reset();
	bool inserted = false;
	std::uint64_t family_bytes = 0;
	if (complete) {
		std::vector<std::byte> payload;
		payload.reserve(harmonic_records_per_family * meter_record_size);
		HarmonicFamilyAssembler assembler;
		std::optional<HarmonicSpectrumSnapshot> snapshot;
		std::uint64_t last_cursor = 0;
		std::int64_t family_measured_at = 0;
		auto records = database.prepare(R"SQL(
SELECT source_fragment,stream_cursor,measured_at_ns,payload
FROM harmonic_pending WHERE period=? AND source_sequence=?
ORDER BY source_fragment
)SQL");
		records.bind(1, period_value(chunk.period));
		records.bind(2, static_cast<std::uint64_t>(chunk.sequence));
		std::uint32_t expected_fragment = 0;
		while (records.step()) {
			if (records.integer(0) != expected_fragment++)
				throw std::invalid_argument(
					"harmonic history fragment sequence is incomplete");
			auto bytes = records.blob(3);
			if (bytes.size() != sizeof(MeterRecord))
				throw std::invalid_argument(
					"harmonic history fragment size is malformed");
			MeterRecord raw{};
			std::memcpy(&raw, bytes.data(), sizeof(raw));
			const auto update = assembler.accept(
				decode_harmonic_record(raw));
			if (update.completed)
				snapshot = update.completed;
			payload.insert(payload.end(), bytes.begin(), bytes.end());
			last_cursor = std::max(last_cursor,
				static_cast<std::uint64_t>(records.integer(1)));
			family_measured_at = std::max(family_measured_at,
				records.integer(2));
		}
		records.reset();
		if (!snapshot || expected_fragment != harmonic_records_per_family)
			throw std::invalid_argument(
				"harmonic history family failed atomic validation");

		auto family = database.prepare(R"SQL(
INSERT OR IGNORE INTO harmonic_families(stream_cursor,period,source_sequence,
 configuration_generation,measured_at_ns,first_sample,sample_count,target_sample,
 contributors,overshoot_samples,status,qualified_max_order,
 first_source_sequence,last_source_sequence,payload)
VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
)SQL");
		family.bind(1, last_cursor);
		family.bind(2, period_value(snapshot->period));
		family.bind(3, static_cast<std::uint64_t>(snapshot->sequence));
		family.bind(4, static_cast<std::uint64_t>(
			snapshot->configuration_generation));
		family.bind(5, family_measured_at);
		family.bind(6, snapshot->first_sample);
		family.bind(7, static_cast<std::uint64_t>(snapshot->sample_count));
		family.bind(8, snapshot->target_sample);
		family.bind(9, static_cast<std::int32_t>(snapshot->contributors));
		family.bind(10,
			static_cast<std::int32_t>(snapshot->overshoot_samples));
		family.bind(11, static_cast<std::uint64_t>(snapshot->status));
		family.bind(12,
			static_cast<std::int32_t>(snapshot->qualified_max_order));
		family.bind(13, static_cast<std::uint64_t>(
			snapshot->first_source_sequence));
		family.bind(14, static_cast<std::uint64_t>(
			snapshot->last_source_sequence));
		family.bind(15, payload);
		family.execute();
		inserted = database.changes() == 1;
		family_bytes = 160u + payload.size();

		auto remove = database.prepare(
			"DELETE FROM harmonic_pending WHERE period=? AND source_sequence=?");
		remove.bind(1, period_value(chunk.period));
		remove.bind(2, static_cast<std::uint64_t>(chunk.sequence));
		remove.execute();
	}
	transaction.commit();
	impl_->acknowledged_cursor = std::max(
		impl_->acknowledged_cursor, stream_cursor);
	if (!inserted)
		return false;

	tracked += family_bytes;
	const auto policy = impl_->manager.policy(dataset);
	if (policy.retention.maximum_age) {
		const auto cutoff = measured_at_ns -
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				*policy.retention.maximum_age).count();
		for (;;) {
			const auto removed = delete_oldest_harmonic_families(database,
				chunk.period, cutoff,
				std::numeric_limits<std::uint64_t>::max());
			if (removed == 0)
				break;
			tracked -= std::min(tracked, removed);
		}
	}
	if (policy.retention.maximum_bytes) {
		while (tracked > *policy.retention.maximum_bytes) {
			const auto removed = delete_oldest_harmonic_families(database,
				chunk.period, std::nullopt,
				tracked - *policy.retention.maximum_bytes);
			if (removed == 0)
				break;
			tracked -= std::min(tracked, removed);
		}
	}
	return true;
}

std::vector<HistoryPoint> MeterHistoryStore::query(const HistoryQuery &query) const
{
	(void)dataset_for(query.period);
	if (query.limit == 0 || query.limit > 50000)
		throw std::invalid_argument("history query limit must be 1..50000");
	if (query.start_nanoseconds > query.end_nanoseconds)
		throw std::invalid_argument("history query start is after its end");
	for (const auto attribute : query.attributes) {
		if (!supported_attribute(attribute))
			throw std::invalid_argument("unsupported history attribute");
	}
	std::scoped_lock lock(impl_->mutex);
	auto &database = impl_->database(query.period);
	std::string sql = R"SQL(
SELECT b.measured_at_ns,v.source_sequence,v.attribute_id,v.signed_value,v.quality
FROM measurement_blocks b JOIN measurement_values v ON v.block_id=b.id
WHERE b.period=? AND b.measured_at_ns>=? AND b.measured_at_ns<=?
)SQL";
	if (!query.attributes.empty()) {
		sql += " AND v.attribute_id IN (";
		for (std::size_t index = 0; index < query.attributes.size(); ++index)
			sql += index == 0 ? "?" : ",?";
		sql += ")";
	}
	sql += " ORDER BY b.measured_at_ns,v.attribute_id LIMIT ?";
	auto statement = database.prepare(sql);
	int parameter = 1;
	statement.bind(parameter++, period_value(query.period));
	statement.bind(parameter++, query.start_nanoseconds);
	statement.bind(parameter++, query.end_nanoseconds);
	for (const auto attribute : query.attributes)
		statement.bind(parameter++, static_cast<std::int32_t>(attribute));
	statement.bind(parameter, static_cast<std::int64_t>(query.limit));
	std::vector<HistoryPoint> result;
	while (statement.step()) {
		const auto id = static_cast<MeterAttributeId>(statement.integer(2));
		result.push_back({statement.integer(0), static_cast<std::uint64_t>(statement.integer(1)),
			id, statement.integer(3), static_cast<MeasurementQuality>(statement.integer(4))});
	}
	return result;
}

HistorianStatus MeterHistoryStore::status() const
{
	std::scoped_lock lock(impl_->mutex);
	HistorianStatus result; result.acknowledged_cursor = impl_->acknowledged_cursor;
	result.storage_bytes = sqlite_family_size(impl_->persistent_path);
	for (const auto &policy : impl_->manager.policies()) {
		if (policy.dataset == mnc::meter_stream::DatabaseDataset::raw_record_spool)
			continue;
		const bool harmonic = harmonic_dataset(policy.dataset);
		const auto period = harmonic ? harmonic_period_for(policy.dataset)
			: period_for(policy.dataset);
		auto &database = harmonic ? impl_->database_for(policy.dataset)
			: impl_->database(period);
		const auto range = dataset_range(database, period, harmonic);
		HistorianStatus::DatasetStatus item;
		item.dataset = policy.dataset; item.backend = policy.backend;
		item.block_count = range.count;
		result.block_count += item.block_count;
		item.oldest_nanoseconds = range.oldest;
		item.newest_nanoseconds = range.newest;
		/* append() and the retention paths maintain these exact totals after
		 * their one-time seed. Reusing them here prevents every UI health poll
		 * from joining and counting all measurement_values in retained history. */
		item.storage_bytes = harmonic
			? impl_->tracked_harmonic_bytes(database, period)
			: impl_->tracked_bytes(database, period);
		if (policy.backend == mnc::meter_stream::StorageBackend::memory)
			result.storage_bytes += item.storage_bytes;
		result.datasets.push_back(item);
	}
	return result;
}

std::vector<mnc::meter_stream::DatabaseStoragePolicy> MeterHistoryStore::policies() const
{ return impl_->manager.policies(); }

std::uint64_t MeterHistoryStore::persisted_stream_high_water() const
{
	std::scoped_lock lock(impl_->mutex);
	auto latest = impl_->persistent.prepare(
		"SELECT MAX(value) FROM ("
		" SELECT COALESCE(MAX(stream_cursor),0) AS value FROM measurement_blocks"
		" UNION ALL SELECT COALESCE(MAX(stream_cursor),0) FROM harmonic_families"
		" UNION ALL SELECT COALESCE(MAX(stream_cursor),0) FROM harmonic_pending"
		")");
	(void)latest.step();
	return static_cast<std::uint64_t>(latest.integer(0));
}

void MeterHistoryStore::prepare_policy_migration(
	const std::vector<mnc::meter_stream::DatabaseStoragePolicy> &policies)
{
	validate_historian_policies(policies);
	std::scoped_lock lock(impl_->mutex);
	for (const auto &policy : policies) {
		const auto current = impl_->manager.policy(policy.dataset);
		if (current.backend == policy.backend ||
		    policy.backend != mnc::meter_stream::StorageBackend::memory)
			continue;
		const bool harmonic = harmonic_dataset(policy.dataset);
		const auto period = harmonic ? harmonic_period_for(policy.dataset)
			: period_for(policy.dataset);
		auto remove = impl_->memory.prepare(harmonic
			? "DELETE FROM harmonic_families WHERE period=?"
			: "DELETE FROM measurement_blocks WHERE period=?");
		remove.bind(1, period_value(period));
		remove.execute();
		if (harmonic) {
			auto pending = impl_->memory.prepare(
				"DELETE FROM harmonic_pending WHERE period=?");
			pending.bind(1, period_value(period));
			pending.execute();
		}
	}
	/* Rows were discarded outside append(), so the cached sizes no longer
	 * describe the tables. Reseeding is a scan, which is why it happens here
	 * in a maintenance window and never on the append path. */
	impl_->forget_tracked_bytes();
}

void MeterHistoryStore::apply_policies(
	std::vector<mnc::meter_stream::DatabaseStoragePolicy> policies)
{
	validate_historian_policies(policies);
	std::scoped_lock lock(impl_->mutex);
	/* The service serializes this routing switch with live ingest, then
	 * backfills the newly selected targets from the durable spool. */
	impl_->manager.apply(std::move(policies));
	/* Routing may now send a period to the other database entirely, so a size
	 * cached against the previous target is meaningless. */
	impl_->forget_tracked_bytes();
}

void MeterHistoryStore::clear_datasets(
	std::span<const mnc::meter_stream::DatabaseDataset> datasets,
	std::uint64_t through_stream_cursor)
{
	std::set<mnc::meter_stream::DatabaseDataset> unique;
	for (const auto dataset : datasets) {
		if (harmonic_dataset(dataset))
			(void)harmonic_period_for(dataset);
		else
			(void)period_for(dataset);
		if (!unique.insert(dataset).second)
			throw std::invalid_argument("duplicate historian dataset clear request");
	}
	if (unique.empty())
		throw std::invalid_argument("historian dataset clear request is empty");

	std::scoped_lock lock(impl_->mutex);
	{
		Transaction transaction(impl_->memory);
		for (const auto dataset : unique) {
			const bool harmonic = harmonic_dataset(dataset);
			const auto period = harmonic ? harmonic_period_for(dataset)
				: period_for(dataset);
			auto remove = impl_->memory.prepare(harmonic
				? "DELETE FROM harmonic_families WHERE period=?"
				: "DELETE FROM measurement_blocks WHERE period=?");
			remove.bind(1, period_value(period));
			remove.execute();
			if (harmonic) {
				auto pending = impl_->memory.prepare(
					"DELETE FROM harmonic_pending WHERE period=?");
				pending.bind(1, period_value(period));
				pending.execute();
			}
		}
		transaction.commit();
	}
	{
		Transaction transaction(impl_->persistent);
		for (const auto dataset : unique) {
			const bool harmonic = harmonic_dataset(dataset);
			const auto period = harmonic ? harmonic_period_for(dataset)
				: period_for(dataset);
			auto remove = impl_->persistent.prepare(harmonic
				? "DELETE FROM harmonic_families WHERE period=?"
				: "DELETE FROM measurement_blocks WHERE period=?");
			remove.bind(1, period_value(period));
			remove.execute();
			if (harmonic) {
				auto pending = impl_->persistent.prepare(
					"DELETE FROM harmonic_pending WHERE period=?");
				pending.bind(1, period_value(period));
				pending.execute();
			}
			write_clear_floor(impl_->persistent, dataset,
				through_stream_cursor);
		}
		transaction.commit();
	}
	for (const auto dataset : unique)
		impl_->clear_floors[dataset] = through_stream_cursor;
	/* Reclaim the pages occupied by explicitly deleted history while the
	 * historian is already in its serialized maintenance window. */
	impl_->persistent.execute("VACUUM");
	impl_->persistent.execute("PRAGMA wal_checkpoint(TRUNCATE)");
	/* Whole datasets were removed outside append(); reseed on next use. */
	impl_->forget_tracked_bytes();
}

void MeterHistoryStore::recreate_database(std::uint64_t through_stream_cursor)
{
	const std::array datasets{
		mnc::meter_stream::DatabaseDataset::basic,
		mnc::meter_stream::DatabaseDataset::cycles_150_180,
		mnc::meter_stream::DatabaseDataset::minutes_10,
		mnc::meter_stream::DatabaseDataset::hours_2,
		mnc::meter_stream::DatabaseDataset::harmonic_cycles_150_180,
		mnc::meter_stream::DatabaseDataset::harmonic_minutes_10,
		mnc::meter_stream::DatabaseDataset::harmonic_hours_2,
	};
	std::scoped_lock lock(impl_->mutex);

	const auto replacement_path = std::filesystem::path(
		impl_->persistent_path.string() + ".fresh");
	remove_database_family(replacement_path);
	{
		Database replacement(replacement_path);
		initialize(replacement, true);
		Transaction transaction(replacement);
		for (const auto dataset : datasets)
			write_clear_floor(replacement, dataset, through_stream_cursor);
		transaction.commit();
		replacement.execute("PRAGMA wal_checkpoint(TRUNCATE)");
	}

	impl_->persistent.execute("PRAGMA wal_checkpoint(TRUNCATE)");
	impl_->persistent = Database(":memory:");
	try {
		/* Both databases were checkpointed and closed above. Remove only
		 * their now-obsolete WAL/SHM files before atomically replacing the
		 * main database; never delete sidecars after the new DB is live. */
		remove_database_sidecars(impl_->persistent_path);
		remove_database_sidecars(replacement_path);
		std::filesystem::rename(replacement_path, impl_->persistent_path);
	} catch (...) {
		impl_->persistent = Database(impl_->persistent_path);
		initialize(impl_->persistent, true);
		remove_database_family(replacement_path);
		throw;
	}

	impl_->persistent = Database(impl_->persistent_path);
	initialize(impl_->persistent, true);
	impl_->memory = Database(":memory:");
	initialize(impl_->memory, false);
	impl_->clear_floors.clear();
	for (const auto dataset : datasets)
		impl_->clear_floors[dataset] = through_stream_cursor;
	/* Both databases were replaced by empty ones. A stale non-zero size here
	 * would make the byte cap delete from a table that has nothing in it. */
	impl_->forget_tracked_bytes();
}

} // namespace msap1::history
