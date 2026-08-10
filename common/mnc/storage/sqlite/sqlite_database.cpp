#include "mnc/storage/sqlite/sqlite_database.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace mnc::storage::sqlite {
namespace {

[[noreturn]] void fail(sqlite3 *database, std::string_view operation)
{
	throw std::runtime_error(std::string(operation) + ": " +
		(database ? sqlite3_errmsg(database) : "SQLite database is closed"));
}

} // namespace

Database::Database(const std::filesystem::path &path)
{
	if (path != std::filesystem::path(":memory:"))
		std::filesystem::create_directories(path.parent_path());
	if (sqlite3_open_v2(path.c_str(), &handle_,
		SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
		nullptr) != SQLITE_OK) {
		const auto message = error_message();
		(void)sqlite3_close_v2(handle_);
		handle_ = nullptr;
		throw std::runtime_error("open SQLite database: " + message);
	}
	(void)sqlite3_busy_timeout(handle_, 5000);
	execute("PRAGMA foreign_keys=ON");
}

Database::~Database()
{
	if (handle_)
		(void)sqlite3_close_v2(handle_);
}

Database::Database(Database &&other) noexcept
	: handle_(std::exchange(other.handle_, nullptr))
{
}

Database &Database::operator=(Database &&other) noexcept
{
	if (this == &other)
		return *this;
	if (handle_)
		(void)sqlite3_close_v2(handle_);
	handle_ = std::exchange(other.handle_, nullptr);
	return *this;
}

void Database::execute(std::string_view sql)
{
	char *message = nullptr;
	const std::string terminated(sql);
	if (sqlite3_exec(handle_, terminated.c_str(), nullptr, nullptr, &message) ==
	    SQLITE_OK)
		return;
	const std::string detail = message ? message : error_message();
	if (message)
		sqlite3_free(message);
	throw std::runtime_error("execute SQLite statement: " + detail);
}

Statement Database::prepare(std::string_view sql) { return Statement(*this, sql); }

std::int64_t Database::last_insert_rowid() const
{
	return sqlite3_last_insert_rowid(handle_);
}

int Database::changes() const { return sqlite3_changes(handle_); }

std::string Database::error_message() const
{
	return handle_ ? sqlite3_errmsg(handle_) : "SQLite database is closed";
}

Statement::Statement(Database &database, std::string_view sql)
	: database_(&database)
{
	const std::string terminated(sql);
	if (sqlite3_prepare_v2(database.native_handle(), terminated.c_str(), -1,
		&statement_, nullptr) != SQLITE_OK)
		fail(database.native_handle(), "prepare SQLite statement");
}

Statement::~Statement()
{
	if (statement_)
		(void)sqlite3_finalize(statement_);
}

Statement::Statement(Statement &&other) noexcept
	: database_(std::exchange(other.database_, nullptr)),
	  statement_(std::exchange(other.statement_, nullptr))
{
}

Statement &Statement::operator=(Statement &&other) noexcept
{
	if (this == &other)
		return *this;
	if (statement_)
		(void)sqlite3_finalize(statement_);
	database_ = std::exchange(other.database_, nullptr);
	statement_ = std::exchange(other.statement_, nullptr);
	return *this;
}

void Statement::check(int result, std::string_view operation) const
{
	if (result != SQLITE_OK)
		fail(database_->native_handle(), operation);
}

void Statement::bind(int index, std::int64_t value)
{
	check(sqlite3_bind_int64(statement_, index, value), "bind SQLite integer");
}

void Statement::bind(int index, std::uint64_t value)
{
	if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
		throw std::overflow_error("SQLite integer exceeds signed 64-bit range");
	bind(index, static_cast<std::int64_t>(value));
}

void Statement::bind(int index, std::int32_t value)
{
	check(sqlite3_bind_int(statement_, index, value), "bind SQLite integer");
}

void Statement::bind(int index, std::string_view value)
{
	check(sqlite3_bind_text(statement_, index, value.data(),
		static_cast<int>(value.size()), SQLITE_TRANSIENT),
		"bind SQLite text");
}

void Statement::bind(int index, std::span<const std::byte> value)
{
	check(sqlite3_bind_blob(statement_, index, value.data(),
		static_cast<int>(value.size()), SQLITE_TRANSIENT),
		"bind SQLite blob");
}

void Statement::bind_null(int index)
{
	check(sqlite3_bind_null(statement_, index), "bind SQLite null");
}

bool Statement::step()
{
	const auto result = sqlite3_step(statement_);
	if (result == SQLITE_ROW)
		return true;
	if (result == SQLITE_DONE)
		return false;
	fail(database_->native_handle(), "step SQLite statement");
}

void Statement::execute()
{
	if (step())
		throw std::runtime_error("SQLite command unexpectedly returned a row");
}

void Statement::reset()
{
	check(sqlite3_reset(statement_), "reset SQLite statement");
	check(sqlite3_clear_bindings(statement_), "clear SQLite bindings");
}

std::int64_t Statement::integer(int column) const
{
	return sqlite3_column_int64(statement_, column);
}

std::string Statement::text(int column) const
{
	const auto *value = sqlite3_column_text(statement_, column);
	return value ? reinterpret_cast<const char *>(value) : std::string{};
}

std::vector<std::byte> Statement::blob(int column) const
{
	const auto size = sqlite3_column_bytes(statement_, column);
	const auto *data = static_cast<const std::byte *>(
		sqlite3_column_blob(statement_, column));
	if (!data || size <= 0)
		return {};
	return {data, data + size};
}

Transaction::Transaction(Database &database) : database_(database)
{
	database_.execute("BEGIN IMMEDIATE");
}

Transaction::~Transaction()
{
	if (!committed_) {
		try {
			database_.execute("ROLLBACK");
		} catch (...) {
		}
	}
}

void Transaction::commit()
{
	database_.execute("COMMIT");
	committed_ = true;
}

} // namespace mnc::storage::sqlite

