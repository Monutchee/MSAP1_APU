#include "mnc/MeterDataProvider/stream/durable_meter_spool.hpp"

#include "mnc/storage/sqlite/sqlite_database.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

namespace mnc::meter_stream {
namespace {

using mnc::storage::sqlite::Database;
using mnc::storage::sqlite::Transaction;

std::int64_t optional_signed(const std::optional<std::int64_t> &value)
{
	return value.value_or(std::numeric_limits<std::int64_t>::min());
}

std::int64_t optional_unsigned(const std::optional<std::uint64_t> &value)
{
	if (!value)
		return std::numeric_limits<std::int64_t>::min();
	if (*value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
		throw std::overflow_error("timestamp uncertainty exceeds SQLite range");
	return static_cast<std::int64_t>(*value);
}

template<typename T>
std::optional<T> from_optional_integer(std::int64_t value)
{
	if (value == std::numeric_limits<std::int64_t>::min())
		return std::nullopt;
	return static_cast<T>(value);
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

/*
 * Cursor lease: consumers of this stream persist state keyed by the cursor
 * (dedup rows, clear floors, acknowledgements), all of which assumes the
 * cursor NEVER repeats a value — including across reboots of a volatile
 * (:memory:) spool, whose AUTOINCREMENT would otherwise restart at 1 and
 * silently collide with every consumer's retained state.  The lease file
 * reserves a cursor range ahead of the newest ever issued, so one small
 * fsync'd write at spool construction (and a rare renewal) buys years of
 * issuance with no per-record I/O.
 */
constexpr std::uint64_t cursor_lease_reservation = std::uint64_t{1} << 31;
constexpr std::uint64_t cursor_lease_renewal_margin = std::uint64_t{1} << 30;

std::filesystem::path cursor_lease_path(const std::filesystem::path &persistent_path)
{
	return std::filesystem::path(persistent_path.string() + ".cursor-lease");
}

/** @return the leased cursor high-water, or 0 when absent or unreadable. */
std::uint64_t read_cursor_lease(const std::filesystem::path &path)
{
	std::ifstream input(path);
	std::uint64_t value = 0;
	if (!(input >> value))
		return 0;
	return value;
}

/*
 * Atomic (temp + fsync + rename) so a power cut can only ever leave the
 * previous lease, never a torn one.  Best-effort by design: a failed write
 * leaves cursors monotonic for this session, and the wall-clock bootstrap in
 * initialize() covers the next one.
 */
bool write_cursor_lease(const std::filesystem::path &path,
			std::uint64_t value) noexcept
{
	std::error_code discard;
	std::filesystem::create_directories(path.parent_path(), discard);
	const auto temporary = std::filesystem::path(path.string() + ".tmp");
	const auto text = std::to_string(value);
	const int fd = ::open(temporary.c_str(),
		O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0640);
	if (fd < 0)
		return false;
	const bool written =
		::write(fd, text.data(), text.size()) ==
			static_cast<ssize_t>(text.size()) &&
		::fsync(fd) == 0;
	(void)::close(fd);
	if (!written)
		return false;
	std::filesystem::rename(temporary, path, discard);
	return !discard;
}

} // namespace

class DurableMeterSpool::Impl final {
public:
	Impl(std::filesystem::path persistent_path, DatabaseStoragePolicy policy)
		: persistent_path(std::move(persistent_path)), policy(policy),
		  database(database_path())
	{
		validate_database_policy(policy);
		if (policy.dataset != DatabaseDataset::raw_record_spool)
			throw std::invalid_argument("spool policy has the wrong dataset");
		initialize();
	}

	std::filesystem::path database_path() const
	{
		return policy.backend == StorageBackend::memory
			? std::filesystem::path(":memory:") : persistent_path;
	}

	void initialize()
	{
		if (policy.backend == StorageBackend::persistent)
			database.execute("PRAGMA journal_mode=WAL");
		database.execute("PRAGMA synchronous=FULL");
		database.execute(R"SQL(
CREATE TABLE IF NOT EXISTS records(
 cursor INTEGER PRIMARY KEY AUTOINCREMENT,
 record_format INTEGER NOT NULL,
 record_kind INTEGER NOT NULL,
 measurement_period INTEGER NOT NULL,
 source_sequence INTEGER NOT NULL,
 source_fragment INTEGER NOT NULL DEFAULT 0,
 configuration_generation INTEGER NOT NULL,
 ingested_at_ns INTEGER NOT NULL,
 first_sample_index INTEGER NOT NULL,
 sample_count INTEGER NOT NULL,
 cycle_count INTEGER NOT NULL,
 time_quality INTEGER NOT NULL,
 utc_start_ns INTEGER NOT NULL,
 utc_uncertainty_ns INTEGER NOT NULL,
 payload BLOB NOT NULL,
 UNIQUE(record_format, configuration_generation, source_sequence, source_fragment,
        first_sample_index)
);
CREATE TABLE IF NOT EXISTS consumers(
 name TEXT PRIMARY KEY,
 acknowledged_cursor INTEGER NOT NULL DEFAULT 0,
 updated_at_ns INTEGER NOT NULL
);
)SQL");
		/* Before M16, the producer identity assumed exactly one record for a
		 * sequence/sample span. Preserve existing rows while widening that key
		 * for bounded multi-record families. */
		bool has_source_fragment = false;
		{
			auto columns = database.prepare("PRAGMA table_info(records)");
			while (columns.step()) {
				if (columns.text(1) == "source_fragment") {
					has_source_fragment = true;
					break;
				}
			}
		}
		if (!has_source_fragment) {
			Transaction migration(database);
			database.execute(R"SQL(
ALTER TABLE records RENAME TO records_without_fragments;
CREATE TABLE records(
 cursor INTEGER PRIMARY KEY AUTOINCREMENT,
 record_format INTEGER NOT NULL,
 record_kind INTEGER NOT NULL,
 measurement_period INTEGER NOT NULL,
 source_sequence INTEGER NOT NULL,
 source_fragment INTEGER NOT NULL DEFAULT 0,
 configuration_generation INTEGER NOT NULL,
 ingested_at_ns INTEGER NOT NULL,
 first_sample_index INTEGER NOT NULL,
 sample_count INTEGER NOT NULL,
 cycle_count INTEGER NOT NULL,
 time_quality INTEGER NOT NULL,
 utc_start_ns INTEGER NOT NULL,
 utc_uncertainty_ns INTEGER NOT NULL,
 payload BLOB NOT NULL,
 UNIQUE(record_format, configuration_generation, source_sequence, source_fragment,
        first_sample_index)
);
INSERT INTO records(
 cursor, record_format, record_kind, measurement_period, source_sequence,
 source_fragment, configuration_generation, ingested_at_ns,
 first_sample_index, sample_count, cycle_count, time_quality, utc_start_ns,
 utc_uncertainty_ns, payload)
SELECT cursor, record_format, record_kind, measurement_period, source_sequence,
 0, configuration_generation, ingested_at_ns, first_sample_index, sample_count,
 cycle_count, time_quality, utc_start_ns, utc_uncertainty_ns, payload
FROM records_without_fragments;
DROP TABLE records_without_fragments;
)SQL");
			migration.commit();
		}
		seed_cursor_space();
		{
			auto usage = database.prepare(
				"SELECT COALESCE(SUM(LENGTH(payload)+128),0) FROM records");
			(void)usage.step();
			tracked_payload_bytes =
				static_cast<std::uint64_t>(usage.integer(0));
		}
	}

	/*
	 * Make the cursor space monotonic across spool sessions.  The seed is the
	 * highest value any past session may have issued: the persisted lease,
	 * whatever this database already contains, and — only when no lease has
	 * ever been written (or it is unreadable) — the wall clock in
	 * milliseconds, which exceeds any count-based cursor a deployed device
	 * can have accumulated and therefore bootstraps upgrades collision-free.
	 */
	void seed_cursor_space()
	{
		const auto lease_file = cursor_lease_path(persistent_path);
		const auto lease = read_cursor_lease(lease_file);
		std::uint64_t newest = 0;
		{
			auto query = database.prepare(
				"SELECT COALESCE(MAX(cursor),0) FROM records");
			(void)query.step();
			newest = static_cast<std::uint64_t>(query.integer(0));
		}
		std::uint64_t seed = std::max(lease, newest);
		if (lease == 0)
			seed = std::max(seed, static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::system_clock::now()
						.time_since_epoch()).count()));
		/* sqlite_sequence exists because records uses AUTOINCREMENT; its
		 * row for the table only appears after a first insert, so create
		 * it if needed, then only ever raise it. */
		{
			auto create = database.prepare(
				"INSERT INTO sqlite_sequence(name,seq) SELECT 'records', ? "
				"WHERE NOT EXISTS(SELECT 1 FROM sqlite_sequence WHERE name='records')");
			create.bind(1, seed);
			create.execute();
		}
		{
			auto raise = database.prepare(
				"UPDATE sqlite_sequence SET seq=? WHERE name='records' AND seq<?");
			raise.bind(1, seed);
			raise.bind(2, seed);
			raise.execute();
		}
		session_start_cursor = seed;
		cursor_lease = seed + cursor_lease_reservation;
		(void)write_cursor_lease(lease_file, cursor_lease);
	}

	/* Renew the on-disk reservation long before issuance can reach it.  Runs
	 * on the acknowledge/prune path — a consumer thread — never on the
	 * producer's publish path. */
	void renew_cursor_lease()
	{
		std::uint64_t newest = 0;
		{
			auto query = database.prepare(
				"SELECT COALESCE(MAX(cursor),0) FROM records");
			(void)query.step();
			newest = static_cast<std::uint64_t>(query.integer(0));
		}
		if (newest + cursor_lease_renewal_margin < cursor_lease)
			return;
		const auto renewed = newest + cursor_lease_reservation;
		if (write_cursor_lease(cursor_lease_path(persistent_path), renewed))
			cursor_lease = renewed;
	}

	std::filesystem::path persistent_path;
	DatabaseStoragePolicy policy;
	Database database;
	mutable std::mutex mutex;
	/** First cursor this spool session can issue; strictly increases across
	 * sessions, so consumers can detect coverage loss (see StreamStatus). */
	std::uint64_t session_start_cursor = 0;
	std::uint64_t cursor_lease = 0;
	/** Running SUM(LENGTH(payload)+128), maintained on insert/delete so the
	 * hard byte cap never costs a table scan on the publish path. */
	std::uint64_t tracked_payload_bytes = 0;
	/** Records evicted by the hard cap before any consumer acknowledged
	 * them — bounded, visible loss instead of unbounded growth. */
	std::uint64_t dropped_unacknowledged = 0;
};

DurableMeterSpool::DurableMeterSpool(std::filesystem::path persistent_path,
				     DatabaseStoragePolicy policy)
	: impl_(std::make_unique<Impl>(std::move(persistent_path), policy))
{
}

DurableMeterSpool::~DurableMeterSpool() = default;

std::uint64_t DurableMeterSpool::publish(const MeterStreamRecord &record)
{
	if (record.payload.empty())
		throw std::invalid_argument("meter stream record payload is empty");
	std::scoped_lock lifecycle(lifecycle_mutex_);
	std::scoped_lock lock(impl_->mutex);
	Transaction transaction(impl_->database);
	/* Check the idempotency key before inserting.  INSERT OR IGNORE on an
	 * AUTOINCREMENT table still consumes a rowid when it encounters a
	 * duplicate, which would create artificial gaps in the public stream
	 * cursor. */
	auto existing = impl_->database.prepare(R"SQL(
SELECT cursor FROM records
 WHERE record_format=? AND configuration_generation=? AND source_sequence=?
   AND source_fragment=? AND first_sample_index=?
)SQL");
	existing.bind(1, static_cast<std::int64_t>(record.record_format));
	existing.bind(2,
		static_cast<std::int64_t>(record.configuration_generation));
	existing.bind(3, record.source_sequence);
	existing.bind(4, static_cast<std::int32_t>(record.source_fragment));
	existing.bind(5, record.timing.first_sample_index);
	if (existing.step()) {
		const auto cursor = static_cast<std::uint64_t>(existing.integer(0));
		/* Never commit with a row-active statement: the WAL auto-checkpoint
		 * runs at COMMIT and aborts while the committing connection still
		 * has one, which is how a WAL grows without bound. */
		existing.reset();
		transaction.commit();
		return cursor;
	}
	auto insert = impl_->database.prepare(R"SQL(
INSERT INTO records(
 record_format, record_kind, measurement_period, source_sequence, source_fragment,
 configuration_generation, ingested_at_ns, first_sample_index, sample_count,
 cycle_count, time_quality, utc_start_ns, utc_uncertainty_ns, payload)
VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
)SQL");
	insert.bind(1, static_cast<std::int64_t>(record.record_format));
	insert.bind(2, static_cast<std::int32_t>(record.record_kind));
	insert.bind(3, static_cast<std::int32_t>(record.measurement_period));
	insert.bind(4, record.source_sequence);
	insert.bind(5, static_cast<std::int32_t>(record.source_fragment));
	insert.bind(6, static_cast<std::int64_t>(record.configuration_generation));
	insert.bind(7, record.ingested_at_nanoseconds);
	insert.bind(8, record.timing.first_sample_index);
	insert.bind(9, static_cast<std::int64_t>(record.timing.sample_count));
	insert.bind(10, static_cast<std::int64_t>(record.timing.cycle_count));
	insert.bind(11, static_cast<std::int32_t>(record.timing.time_quality));
	insert.bind(12, optional_signed(record.timing.utc_start_nanoseconds));
	insert.bind(13, optional_unsigned(record.timing.utc_uncertainty_nanoseconds));
	insert.bind(14, record.payload);
	insert.execute();

	const auto cursor = static_cast<std::uint64_t>(
		impl_->database.last_insert_rowid());
	impl_->tracked_payload_bytes += record.payload.size() + 128u;
	/*
	 * Hard cap, enforced HERE and not only in prune(): prune() runs from the
	 * acknowledge handler, which is exactly what stops arriving when the
	 * consumer wedges — the one scenario an unbounded spool must survive.
	 * Oldest records go first, acknowledged or not; unacknowledged evictions
	 * are counted so the loss is bounded AND visible, which beats the
	 * alternative (an OOM-killed stream service that takes the producer's
	 * publish path down with it).
	 */
	if (impl_->policy.retention.maximum_bytes &&
	    impl_->tracked_payload_bytes > *impl_->policy.retention.maximum_bytes) {
		std::uint64_t acknowledged = 0;
		{
			auto minimum = impl_->database.prepare(
				"SELECT COALESCE(MIN(acknowledged_cursor),0) FROM consumers");
			(void)minimum.step();
			acknowledged = static_cast<std::uint64_t>(minimum.integer(0));
		}
		std::uint64_t remove_through = 0;
		std::uint64_t removed_bytes = 0;
		std::uint64_t removed_unacknowledged = 0;
		{
			/* The record just published is excluded: a cap smaller than
			 * one record must degrade to keeping the newest, never to an
			 * empty stream. */
			auto victims = impl_->database.prepare(
				"SELECT cursor, LENGTH(payload)+128 FROM records "
				"WHERE cursor<? ORDER BY cursor");
			victims.bind(1, cursor);
			while (impl_->tracked_payload_bytes - removed_bytes >
				       *impl_->policy.retention.maximum_bytes &&
			       victims.step()) {
				remove_through = static_cast<std::uint64_t>(
					victims.integer(0));
				removed_bytes += static_cast<std::uint64_t>(
					victims.integer(1));
				if (remove_through > acknowledged)
					++removed_unacknowledged;
			}
		}
		if (remove_through != 0) {
			auto remove = impl_->database.prepare(
				"DELETE FROM records WHERE cursor<=?");
			remove.bind(1, remove_through);
			remove.execute();
			impl_->tracked_payload_bytes -=
				std::min(impl_->tracked_payload_bytes, removed_bytes);
			impl_->dropped_unacknowledged += removed_unacknowledged;
		}
	}
	transaction.commit();
	return cursor;
}

void DurableMeterSpool::register_consumer(std::string_view name)
{
	if (name.empty())
		throw std::invalid_argument("meter stream consumer name is empty");
	std::scoped_lock lifecycle(lifecycle_mutex_);
	std::scoped_lock lock(impl_->mutex);
	auto statement = impl_->database.prepare(R"SQL(
INSERT INTO consumers(name, acknowledged_cursor, updated_at_ns)
VALUES(?, 0, ?)
ON CONFLICT(name) DO UPDATE SET updated_at_ns=excluded.updated_at_ns
)SQL");
	statement.bind(1, name);
	statement.bind(2, std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count());
	statement.execute();
}

void DurableMeterSpool::unregister_consumer(std::string_view name)
{
	if (name.empty())
		throw std::invalid_argument("meter stream consumer name is empty");
	std::scoped_lock lifecycle(lifecycle_mutex_);
	std::scoped_lock lock(impl_->mutex);
	auto statement = impl_->database.prepare(
		"DELETE FROM consumers WHERE name=?");
	statement.bind(1, name); statement.execute();
}

std::vector<MeterStreamRecord> DurableMeterSpool::read_after(
	std::string_view name, std::size_t limit)
{
	if (limit == 0 || limit > 4096)
		throw std::invalid_argument("meter stream read limit must be 1..4096");
	std::scoped_lock lifecycle(lifecycle_mutex_);
	std::scoped_lock lock(impl_->mutex);
	auto consumer = impl_->database.prepare(
		"SELECT acknowledged_cursor FROM consumers WHERE name=?");
	consumer.bind(1, name);
	if (!consumer.step())
		throw std::invalid_argument("meter stream consumer is not registered");
	const auto acknowledged = consumer.integer(0);
	auto query = impl_->database.prepare(R"SQL(
SELECT cursor, record_format, record_kind, measurement_period,
 source_sequence, source_fragment, configuration_generation, ingested_at_ns,
 first_sample_index, sample_count, cycle_count, time_quality,
 utc_start_ns, utc_uncertainty_ns, payload
FROM records WHERE cursor>? ORDER BY cursor LIMIT ?
)SQL");
	query.bind(1, acknowledged);
	query.bind(2, static_cast<std::int64_t>(limit));
	std::vector<MeterStreamRecord> result;
	while (query.step()) {
		MeterStreamRecord record;
		record.cursor = static_cast<std::uint64_t>(query.integer(0));
		record.record_format = static_cast<std::uint32_t>(query.integer(1));
		record.record_kind = static_cast<std::uint16_t>(query.integer(2));
		record.measurement_period = static_cast<std::uint8_t>(query.integer(3));
		record.source_sequence = static_cast<std::uint64_t>(query.integer(4));
		record.source_fragment = static_cast<std::uint16_t>(query.integer(5));
		record.configuration_generation = static_cast<std::uint32_t>(query.integer(6));
		record.ingested_at_nanoseconds = query.integer(7);
		record.timing.first_sample_index = static_cast<std::uint64_t>(query.integer(8));
		record.timing.sample_count = static_cast<std::uint32_t>(query.integer(9));
		record.timing.cycle_count = static_cast<std::uint32_t>(query.integer(10));
		record.timing.time_quality = static_cast<std::uint8_t>(query.integer(11));
		record.timing.utc_start_nanoseconds =
			from_optional_integer<std::int64_t>(query.integer(12));
		record.timing.utc_uncertainty_nanoseconds =
			from_optional_integer<std::uint64_t>(query.integer(13));
		record.payload = query.blob(14);
		result.push_back(std::move(record));
	}
	return result;
}

void DurableMeterSpool::acknowledge(std::string_view name, std::uint64_t cursor)
{
	std::scoped_lock lifecycle(lifecycle_mutex_);
	std::scoped_lock lock(impl_->mutex);
	auto newest = impl_->database.prepare("SELECT COALESCE(MAX(cursor),0) FROM records");
	(void)newest.step();
	const auto head = static_cast<std::uint64_t>(newest.integer(0));
	/* Row-active statements block the WAL auto-checkpoint of the UPDATE's
	 * autocommit below; release the snapshot first. */
	newest.reset();
	if (cursor > head)
		throw std::invalid_argument("consumer acknowledgement exceeds stream head");
	auto update = impl_->database.prepare(R"SQL(
UPDATE consumers SET acknowledged_cursor=MAX(acknowledged_cursor,?),
 updated_at_ns=? WHERE name=?
)SQL");
	update.bind(1, cursor);
	update.bind(2, std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count());
	update.bind(3, name);
	update.execute();
	if (impl_->database.changes() != 1)
		throw std::invalid_argument("meter stream consumer is not registered");
}

StreamStatus DurableMeterSpool::status() const
{
	std::scoped_lock lifecycle(lifecycle_mutex_);
	std::scoped_lock lock(impl_->mutex);
	auto query = impl_->database.prepare(
		"SELECT COALESCE(MIN(cursor),0), COALESCE(MAX(cursor),0), COUNT(*) FROM records");
	(void)query.step();
	StreamStatus result;
	result.durability = impl_->policy.backend == StorageBackend::persistent;
	result.oldest_cursor = static_cast<std::uint64_t>(query.integer(0));
	result.newest_cursor = static_cast<std::uint64_t>(query.integer(1));
	result.record_count = static_cast<std::uint64_t>(query.integer(2));
	result.session_start_cursor = impl_->session_start_cursor;
	result.dropped_unacknowledged_records = impl_->dropped_unacknowledged;
	if (result.durability) {
		result.storage_bytes = sqlite_family_size(impl_->persistent_path);
	}
	auto consumers = impl_->database.prepare(
		"SELECT name,acknowledged_cursor FROM consumers ORDER BY name");
	while (consumers.step())
		result.consumers.push_back({consumers.text(0),
			static_cast<std::uint64_t>(consumers.integer(1))});
	return result;
}

DatabaseStoragePolicy DurableMeterSpool::policy() const
{
	std::scoped_lock lifecycle(lifecycle_mutex_);
	std::scoped_lock lock(impl_->mutex);
	return impl_->policy;
}

std::uint64_t DurableMeterSpool::dropped_unacknowledged_records() const
{
	std::scoped_lock lifecycle(lifecycle_mutex_);
	std::scoped_lock lock(impl_->mutex);
	return impl_->dropped_unacknowledged;
}

void DurableMeterSpool::apply_policy(DatabaseStoragePolicy policy)
{
	validate_database_policy(policy);
	if (policy.dataset != DatabaseDataset::raw_record_spool)
		throw std::invalid_argument("spool policy has the wrong dataset");
	std::scoped_lock lifecycle(lifecycle_mutex_);
	std::unique_lock source_lock(impl_->mutex);
	if (policy.backend == impl_->policy.backend) {
		impl_->policy = policy;
		return;
	}

	/* Build the replacement database completely before publishing it.  The
	 * lifecycle lock serializes producers and consumers while the source is
	 * copied, so there is no window where a committed record can disappear. */
	auto replacement = std::make_unique<Impl>(impl_->persistent_path, policy);
	std::unique_lock target_lock(replacement->mutex);
	Transaction transaction(replacement->database);
	/* The target is a replacement for the active stream, not an archive to
	 * merge. In particular, returning from a volatile test backend must not
	 * resurrect records or consumer cursors left in an older on-disk spool. */
	replacement->database.execute("DELETE FROM records; DELETE FROM consumers");
	auto records = impl_->database.prepare(R"SQL(
SELECT cursor,record_format,record_kind,measurement_period,source_sequence,
 source_fragment,configuration_generation,ingested_at_ns,first_sample_index,sample_count,
 cycle_count,time_quality,utc_start_ns,utc_uncertainty_ns,payload
FROM records ORDER BY cursor
)SQL");
	auto insert = replacement->database.prepare(R"SQL(
INSERT OR IGNORE INTO records(cursor,record_format,record_kind,measurement_period,
 source_sequence,source_fragment,configuration_generation,ingested_at_ns,first_sample_index,
 sample_count,cycle_count,time_quality,utc_start_ns,utc_uncertainty_ns,payload)
VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
)SQL");
	while (records.step()) {
		for (int column = 0; column < 14; ++column)
			insert.bind(column + 1, records.integer(column));
		insert.bind(15, records.blob(14));
		insert.execute();
		insert.reset();
	}
	auto consumers = impl_->database.prepare(
		"SELECT name,acknowledged_cursor,updated_at_ns FROM consumers");
	auto add_consumer = replacement->database.prepare(
		"INSERT INTO consumers(name,acknowledged_cursor,updated_at_ns) VALUES(?,?,?) "
		"ON CONFLICT(name) DO UPDATE SET "
		"acknowledged_cursor=MAX(acknowledged_cursor,excluded.acknowledged_cursor),"
		"updated_at_ns=excluded.updated_at_ns");
	while (consumers.step()) {
		add_consumer.bind(1, consumers.text(0));
		add_consumer.bind(2, consumers.integer(1));
		add_consumer.bind(3, consumers.integer(2));
		add_consumer.execute();
		add_consumer.reset();
	}
	/* The replacement seeded its byte accounting from the (then-empty or
	 * stale) target database before this copy; recount the migrated rows. */
	{
		auto usage = replacement->database.prepare(
			"SELECT COALESCE(SUM(LENGTH(payload)+128),0) FROM records");
		(void)usage.step();
		replacement->tracked_payload_bytes =
			static_cast<std::uint64_t>(usage.integer(0));
	}
	/* Eviction loss is a property of the stream, not of one backend. */
	replacement->dropped_unacknowledged = impl_->dropped_unacknowledged;
	transaction.commit();
	target_lock.unlock();
	impl_.swap(replacement);
	source_lock.unlock();
}

void DurableMeterSpool::prune()
{
	std::scoped_lock lifecycle(lifecycle_mutex_);
	std::scoped_lock lock(impl_->mutex);
	/* This runs on a consumer thread, so it is where the cursor-lease
	 * reservation is renewed without touching the publish path. */
	impl_->renew_cursor_lease();
	if (!impl_->policy.retention.maximum_age &&
	    !impl_->policy.retention.maximum_bytes)
		return;
	std::int64_t acknowledged = 0;
	{
		/* Statements must not stay row-active across the autocommitting
		 * DELETEs below — an active statement makes the commit-time WAL
		 * auto-checkpoint abort, and the WAL then grows without bound. */
		auto minimum = impl_->database.prepare(
			"SELECT MIN(acknowledged_cursor) FROM consumers");
		if (!minimum.step())
			return;
		acknowledged = minimum.integer(0);
	}
	if (impl_->policy.retention.maximum_age) {
		const auto cutoff = std::chrono::duration_cast<std::chrono::nanoseconds>(
			(std::chrono::system_clock::now() -
			 *impl_->policy.retention.maximum_age).time_since_epoch()).count();
		std::uint64_t removed_bytes = 0;
		{
			auto victims = impl_->database.prepare(
				"SELECT COALESCE(SUM(LENGTH(payload)+128),0) FROM records "
				"WHERE cursor<=? AND ingested_at_ns<?");
			victims.bind(1, acknowledged);
			victims.bind(2, cutoff);
			(void)victims.step();
			removed_bytes = static_cast<std::uint64_t>(victims.integer(0));
		}
		auto remove = impl_->database.prepare(
			"DELETE FROM records WHERE cursor<=? AND ingested_at_ns<?");
		remove.bind(1, acknowledged);
		remove.bind(2, cutoff);
		remove.execute();
		impl_->tracked_payload_bytes -=
			std::min(impl_->tracked_payload_bytes, removed_bytes);
	}
	if (impl_->policy.retention.maximum_bytes &&
	    impl_->tracked_payload_bytes > *impl_->policy.retention.maximum_bytes) {
		auto bytes = impl_->tracked_payload_bytes;
		std::uint64_t remove_through = 0;
		{
			auto eligible = impl_->database.prepare(
				"SELECT cursor,LENGTH(payload)+128 FROM records WHERE cursor<=? ORDER BY cursor");
			eligible.bind(1, acknowledged);
			while (bytes > *impl_->policy.retention.maximum_bytes && eligible.step()) {
				remove_through = static_cast<std::uint64_t>(eligible.integer(0));
				bytes -= std::min(bytes,
					static_cast<std::uint64_t>(eligible.integer(1)));
			}
		}
		if (remove_through != 0) {
			auto remove = impl_->database.prepare(
				"DELETE FROM records WHERE cursor<=?");
			remove.bind(1, remove_through);
			remove.execute();
			impl_->tracked_payload_bytes = bytes;
		}
	}
}

} // namespace mnc::meter_stream
