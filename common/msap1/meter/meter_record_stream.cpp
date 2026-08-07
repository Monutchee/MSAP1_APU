#include "msap1/meter/meter_record_stream.hpp"

#include <cstring>
#include <mutex>
#include <stdexcept>
#include <utility>

#include <dlfcn.h>

/* Keep the reusable APU build independent of a host sqlite development
 * package. Yocto supplies libsqlite3 at build/runtime; host tests resolve the
 * same stable C ABI dynamically from libsqlite3.so.0. */
struct sqlite3;
struct sqlite3_stmt;

namespace {
inline constexpr int sqlite_ok = 0;
inline constexpr int sqlite_row = 100;
inline constexpr int sqlite_done = 101;
inline constexpr int sqlite_open_readwrite = 0x00000002;
inline constexpr int sqlite_open_create = 0x00000004;
inline constexpr int sqlite_open_fullmutex = 0x00010000;
inline constexpr int sqlite_transient_value = -1;
using sqlite_destructor = void (*)(void *);

struct SqliteApi {
	void *library = nullptr;
	int (*open_v2)(const char *, sqlite3 **, int, const char *) = nullptr;
	int (*close_v2)(sqlite3 *) = nullptr;
	const char *(*errmsg)(sqlite3 *) = nullptr;
	int (*exec)(sqlite3 *, const char *, int (*)(void *, int, char **, char **),
		    void *, char **) = nullptr;
	void (*free_memory)(void *) = nullptr;
	int (*prepare_v2)(sqlite3 *, const char *, int, sqlite3_stmt **,
			  const char **) = nullptr;
	int (*finalize)(sqlite3_stmt *) = nullptr;
	int (*step)(sqlite3_stmt *) = nullptr;
	int (*reset)(sqlite3_stmt *) = nullptr;
	int (*clear_bindings)(sqlite3_stmt *) = nullptr;
	int (*bind_int64)(sqlite3_stmt *, int, long long) = nullptr;
	int (*bind_text)(sqlite3_stmt *, int, const char *, int,
			 sqlite_destructor) = nullptr;
	int (*bind_blob)(sqlite3_stmt *, int, const void *, int,
			 sqlite_destructor) = nullptr;
	long long (*last_insert_rowid)(sqlite3 *) = nullptr;
	long long (*column_int64)(sqlite3_stmt *, int) = nullptr;
	const void *(*column_blob)(sqlite3_stmt *, int) = nullptr;
	int (*column_bytes)(sqlite3_stmt *, int) = nullptr;
	int (*changes)(sqlite3 *) = nullptr;

	template<typename T>
	void load(T &target, const char *name)
	{
		target = reinterpret_cast<T>(::dlsym(library, name));
		if (!target)
			throw std::runtime_error(std::string("missing sqlite symbol ") + name);
	}

	SqliteApi()
	{
		library = ::dlopen("libsqlite3.so.0", RTLD_NOW | RTLD_LOCAL);
		if (!library)
			library = ::dlopen("libsqlite3.so", RTLD_NOW | RTLD_LOCAL);
		if (!library)
			throw std::runtime_error("libsqlite3 is unavailable");
		load(open_v2, "sqlite3_open_v2");
		load(close_v2, "sqlite3_close_v2");
		load(errmsg, "sqlite3_errmsg");
		load(exec, "sqlite3_exec");
		load(free_memory, "sqlite3_free");
		load(prepare_v2, "sqlite3_prepare_v2");
		load(finalize, "sqlite3_finalize");
		load(step, "sqlite3_step");
		load(reset, "sqlite3_reset");
		load(clear_bindings, "sqlite3_clear_bindings");
		load(bind_int64, "sqlite3_bind_int64");
		load(bind_text, "sqlite3_bind_text");
		load(bind_blob, "sqlite3_bind_blob");
		load(last_insert_rowid, "sqlite3_last_insert_rowid");
		load(column_int64, "sqlite3_column_int64");
		load(column_blob, "sqlite3_column_blob");
		load(column_bytes, "sqlite3_column_bytes");
		load(changes, "sqlite3_changes");
	}

	~SqliteApi()
	{
		if (library)
			::dlclose(library);
	}
};

SqliteApi &sqlite()
{
	static SqliteApi api;
	return api;
}

struct Statement {
	sqlite3_stmt *value = nullptr;
	Statement() = default;
	Statement(const Statement &) = delete;
	Statement &operator=(const Statement &) = delete;
	Statement(Statement &&other) noexcept
		: value(std::exchange(other.value, nullptr))
	{
	}
	Statement &operator=(Statement &&other) noexcept
	{
		if (this != &other) {
			if (value)
				sqlite().finalize(value);
			value = std::exchange(other.value, nullptr);
		}
		return *this;
	}
	~Statement()
	{
		if (value)
			sqlite().finalize(value);
	}
};

} // namespace

namespace msap1 {

struct MeterRecordStream::Impl {
	std::filesystem::path path;
	sqlite3 *database = nullptr;
	mutable std::mutex mutex;

	~Impl()
	{
		if (database)
			sqlite().close_v2(database);
	}

	[[noreturn]] void fail(const std::string &operation) const
	{
		throw std::runtime_error(operation + ": " + sqlite().errmsg(database));
	}

	void execute(const char *sql) const
	{
		char *message = nullptr;
		if (sqlite().exec(database, sql, nullptr, nullptr, &message) != sqlite_ok) {
			const std::string detail = message ? message : sqlite().errmsg(database);
			if (message)
				sqlite().free_memory(message);
			throw std::runtime_error("sqlite execution failed: " + detail);
		}
	}

	Statement prepare(const char *sql) const
	{
		Statement statement;
		if (sqlite().prepare_v2(database, sql, -1, &statement.value, nullptr) !=
		    sqlite_ok)
			fail("prepare sqlite statement");
		return statement;
	}
};

MeterRecordStream::MeterRecordStream(std::filesystem::path database_path)
	: impl_(std::make_unique<Impl>())
{
	impl_->path = std::move(database_path);
	if (!impl_->path.parent_path().empty())
		std::filesystem::create_directories(impl_->path.parent_path());
	if (sqlite().open_v2(impl_->path.c_str(), &impl_->database,
			     sqlite_open_readwrite | sqlite_open_create |
				     sqlite_open_fullmutex,
			     nullptr) != sqlite_ok)
		impl_->fail("open meter record stream");
	impl_->execute("PRAGMA journal_mode=WAL;");
	impl_->execute("PRAGMA synchronous=FULL;");
	impl_->execute("PRAGMA foreign_keys=ON;");
	impl_->execute(
		"CREATE TABLE IF NOT EXISTS meter_records("
		"cursor INTEGER PRIMARY KEY AUTOINCREMENT,"
		"received_ns INTEGER NOT NULL,"
		"source_sequence INTEGER NOT NULL,"
		"record BLOB NOT NULL);"
		"CREATE TABLE IF NOT EXISTS meter_consumers("
		"name TEXT PRIMARY KEY,"
		"ack_cursor INTEGER NOT NULL DEFAULT 0);"
		"CREATE INDEX IF NOT EXISTS meter_records_received "
		"ON meter_records(received_ns);");
}

MeterRecordStream::~MeterRecordStream() = default;
MeterRecordStream::MeterRecordStream(MeterRecordStream &&) noexcept = default;
MeterRecordStream &MeterRecordStream::operator=(MeterRecordStream &&) noexcept =
	default;

MeterCursor MeterRecordStream::append(
	const MeterRecord &record, std::chrono::system_clock::time_point received_at)
{
	std::scoped_lock lock(impl_->mutex);
	impl_->execute("BEGIN IMMEDIATE;");
	try {
		auto statement = impl_->prepare(
			"INSERT INTO meter_records(received_ns,source_sequence,record) "
			"VALUES(?1,?2,?3);");
		const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(
			received_at.time_since_epoch()).count();
		if (sqlite().bind_int64(statement.value, 1, nanoseconds) != sqlite_ok ||
		    sqlite().bind_int64(statement.value, 2, record.sequence()) != sqlite_ok ||
		    sqlite().bind_blob(statement.value, 3, &record, sizeof(record),
				       reinterpret_cast<sqlite_destructor>(
					       sqlite_transient_value)) != sqlite_ok ||
		    sqlite().step(statement.value) != sqlite_done)
			impl_->fail("append meter record");
		const auto cursor = static_cast<MeterCursor>(
			sqlite().last_insert_rowid(impl_->database));
		impl_->execute("COMMIT;");
		return cursor;
	} catch (...) {
		try {
			impl_->execute("ROLLBACK;");
		} catch (...) {
		}
		throw;
	}
}

std::vector<StoredMeterRecord>
MeterRecordStream::read_after(MeterCursor cursor, std::size_t limit) const
{
	if (limit == 0 || limit > 65536)
		throw std::invalid_argument("meter stream read limit is invalid");
	std::scoped_lock lock(impl_->mutex);
	auto statement = impl_->prepare(
		"SELECT cursor,received_ns,record FROM meter_records "
		"WHERE cursor>?1 ORDER BY cursor LIMIT ?2;");
	sqlite().bind_int64(statement.value, 1, static_cast<long long>(cursor));
	sqlite().bind_int64(statement.value, 2, static_cast<long long>(limit));
	std::vector<StoredMeterRecord> result;
	while (true) {
		const auto status = sqlite().step(statement.value);
		if (status == sqlite_done)
			break;
		if (status != sqlite_row)
			impl_->fail("read meter record stream");
		if (sqlite().column_bytes(statement.value, 2) != sizeof(MeterRecord))
			throw std::runtime_error("stored meter record has invalid size");
		StoredMeterRecord item{};
		item.cursor = static_cast<MeterCursor>(
			sqlite().column_int64(statement.value, 0));
		item.received_at = std::chrono::system_clock::time_point{
			std::chrono::nanoseconds{
				sqlite().column_int64(statement.value, 1)}};
		std::memcpy(&item.record, sqlite().column_blob(statement.value, 2),
			    sizeof(item.record));
		result.push_back(item);
	}
	return result;
}

void MeterRecordStream::register_consumer(const std::string &name)
{
	if (name.empty())
		throw std::invalid_argument("meter consumer name is empty");
	std::scoped_lock lock(impl_->mutex);
	auto statement = impl_->prepare(
		"INSERT OR IGNORE INTO meter_consumers(name,ack_cursor) VALUES(?1,0);");
	sqlite().bind_text(statement.value, 1, name.c_str(),
			   static_cast<int>(name.size()),
			   reinterpret_cast<sqlite_destructor>(sqlite_transient_value));
	if (sqlite().step(statement.value) != sqlite_done)
		impl_->fail("register meter stream consumer");
}

void MeterRecordStream::acknowledge(const std::string &name, MeterCursor cursor)
{
	std::scoped_lock lock(impl_->mutex);
	auto statement = impl_->prepare(
		"UPDATE meter_consumers SET ack_cursor=max(ack_cursor,?2) WHERE name=?1;");
	sqlite().bind_text(statement.value, 1, name.c_str(),
			   static_cast<int>(name.size()),
			   reinterpret_cast<sqlite_destructor>(sqlite_transient_value));
	sqlite().bind_int64(statement.value, 2, static_cast<long long>(cursor));
	if (sqlite().step(statement.value) != sqlite_done)
		impl_->fail("acknowledge meter stream consumer");
	if (sqlite().changes(impl_->database) == 0)
		throw std::invalid_argument("unknown meter stream consumer");
}

std::size_t MeterRecordStream::prune(std::chrono::hours safety_window)
{
	std::scoped_lock lock(impl_->mutex);
	const auto cutoff = std::chrono::duration_cast<std::chrono::nanoseconds>(
		(std::chrono::system_clock::now() - safety_window).time_since_epoch())
				.count();
	auto statement = impl_->prepare(
		"DELETE FROM meter_records WHERE received_ns<?1 AND cursor<="
		"(SELECT COALESCE(MIN(ack_cursor),0) FROM meter_consumers);");
	sqlite().bind_int64(statement.value, 1, cutoff);
	if (sqlite().step(statement.value) != sqlite_done)
		impl_->fail("prune meter record stream");
	return static_cast<std::size_t>(sqlite().changes(impl_->database));
}

const std::filesystem::path &MeterRecordStream::path() const noexcept
{
	return impl_->path;
}

} // namespace msap1
