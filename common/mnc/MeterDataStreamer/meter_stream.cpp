#include "mnc/MeterDataStreamer/meter_stream.hpp"

#include "mnc/storage/sqlite/sqlite_database.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

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
 configuration_generation INTEGER NOT NULL,
 ingested_at_ns INTEGER NOT NULL,
 first_sample_index INTEGER NOT NULL,
 sample_count INTEGER NOT NULL,
 cycle_count INTEGER NOT NULL,
 time_quality INTEGER NOT NULL,
 utc_start_ns INTEGER NOT NULL,
 utc_uncertainty_ns INTEGER NOT NULL,
 payload BLOB NOT NULL,
 UNIQUE(record_format, configuration_generation, source_sequence,
        first_sample_index)
);
CREATE TABLE IF NOT EXISTS consumers(
 name TEXT PRIMARY KEY,
 acknowledged_cursor INTEGER NOT NULL DEFAULT 0,
 updated_at_ns INTEGER NOT NULL
);
)SQL");
	}

	std::filesystem::path persistent_path;
	DatabaseStoragePolicy policy;
	Database database;
	mutable std::mutex mutex;
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
   AND first_sample_index=?
)SQL");
	existing.bind(1, static_cast<std::int64_t>(record.record_format));
	existing.bind(2,
		static_cast<std::int64_t>(record.configuration_generation));
	existing.bind(3, record.source_sequence);
	existing.bind(4, record.timing.first_sample_index);
	if (existing.step()) {
		const auto cursor = static_cast<std::uint64_t>(existing.integer(0));
		transaction.commit();
		return cursor;
	}
	auto insert = impl_->database.prepare(R"SQL(
INSERT INTO records(
 record_format, record_kind, measurement_period, source_sequence,
 configuration_generation, ingested_at_ns, first_sample_index, sample_count,
 cycle_count, time_quality, utc_start_ns, utc_uncertainty_ns, payload)
VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
)SQL");
	insert.bind(1, static_cast<std::int64_t>(record.record_format));
	insert.bind(2, static_cast<std::int32_t>(record.record_kind));
	insert.bind(3, static_cast<std::int32_t>(record.measurement_period));
	insert.bind(4, record.source_sequence);
	insert.bind(5, static_cast<std::int64_t>(record.configuration_generation));
	insert.bind(6, record.ingested_at_nanoseconds);
	insert.bind(7, record.timing.first_sample_index);
	insert.bind(8, static_cast<std::int64_t>(record.timing.sample_count));
	insert.bind(9, static_cast<std::int64_t>(record.timing.cycle_count));
	insert.bind(10, static_cast<std::int32_t>(record.timing.time_quality));
	insert.bind(11, optional_signed(record.timing.utc_start_nanoseconds));
	insert.bind(12, optional_unsigned(record.timing.utc_uncertainty_nanoseconds));
	insert.bind(13, record.payload);
	insert.execute();

	const auto cursor = static_cast<std::uint64_t>(
		impl_->database.last_insert_rowid());
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
 source_sequence, configuration_generation, ingested_at_ns,
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
		record.configuration_generation = static_cast<std::uint32_t>(query.integer(5));
		record.ingested_at_nanoseconds = query.integer(6);
		record.timing.first_sample_index = static_cast<std::uint64_t>(query.integer(7));
		record.timing.sample_count = static_cast<std::uint32_t>(query.integer(8));
		record.timing.cycle_count = static_cast<std::uint32_t>(query.integer(9));
		record.timing.time_quality = static_cast<std::uint8_t>(query.integer(10));
		record.timing.utc_start_nanoseconds =
			from_optional_integer<std::int64_t>(query.integer(11));
		record.timing.utc_uncertainty_nanoseconds =
			from_optional_integer<std::uint64_t>(query.integer(12));
		record.payload = query.blob(13);
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
	if (cursor > static_cast<std::uint64_t>(newest.integer(0)))
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
 configuration_generation,ingested_at_ns,first_sample_index,sample_count,
 cycle_count,time_quality,utc_start_ns,utc_uncertainty_ns,payload
FROM records ORDER BY cursor
)SQL");
	auto insert = replacement->database.prepare(R"SQL(
INSERT OR IGNORE INTO records(cursor,record_format,record_kind,measurement_period,
 source_sequence,configuration_generation,ingested_at_ns,first_sample_index,
 sample_count,cycle_count,time_quality,utc_start_ns,utc_uncertainty_ns,payload)
VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)
)SQL");
	while (records.step()) {
		for (int column = 0; column < 13; ++column)
			insert.bind(column + 1, records.integer(column));
		insert.bind(14, records.blob(13));
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
	transaction.commit();
	target_lock.unlock();
	impl_.swap(replacement);
	source_lock.unlock();
}

void DurableMeterSpool::prune()
{
	std::scoped_lock lifecycle(lifecycle_mutex_);
	std::scoped_lock lock(impl_->mutex);
	if (!impl_->policy.retention.maximum_age &&
	    !impl_->policy.retention.maximum_bytes)
		return;
	auto minimum = impl_->database.prepare(
		"SELECT MIN(acknowledged_cursor) FROM consumers");
	if (!minimum.step())
		return;
	const auto acknowledged = minimum.integer(0);
	if (impl_->policy.retention.maximum_age) {
		const auto cutoff = std::chrono::duration_cast<std::chrono::nanoseconds>(
			(std::chrono::system_clock::now() -
			 *impl_->policy.retention.maximum_age).time_since_epoch()).count();
		auto remove = impl_->database.prepare(
			"DELETE FROM records WHERE cursor<=? AND ingested_at_ns<?");
		remove.bind(1, acknowledged);
		remove.bind(2, cutoff);
		remove.execute();
	}
	if (impl_->policy.retention.maximum_bytes) {
		auto usage = impl_->database.prepare(
			"SELECT COALESCE(SUM(LENGTH(payload)+128),0) FROM records");
		(void)usage.step();
		auto bytes = static_cast<std::uint64_t>(usage.integer(0));
		if (bytes > *impl_->policy.retention.maximum_bytes) {
			auto eligible = impl_->database.prepare(
				"SELECT cursor,LENGTH(payload)+128 FROM records WHERE cursor<=? ORDER BY cursor");
			eligible.bind(1, acknowledged);
			std::uint64_t remove_through = 0;
			while (bytes > *impl_->policy.retention.maximum_bytes && eligible.step()) {
				remove_through = static_cast<std::uint64_t>(eligible.integer(0));
				bytes -= std::min(bytes,
					static_cast<std::uint64_t>(eligible.integer(1)));
			}
			if (remove_through != 0) {
				auto remove = impl_->database.prepare(
					"DELETE FROM records WHERE cursor<=?");
				remove.bind(1, remove_through);
				remove.execute();
			}
		}
	}
}

} // namespace mnc::meter_stream
