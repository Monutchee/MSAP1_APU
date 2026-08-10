#pragma once

#include "mnc/storage/sqlite/sqlite_compat.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mnc::storage::sqlite {

class Database;

/** Move-only prepared statement with checked bindings and stepping. */
class Statement final {
public:
	Statement() = default;
	Statement(Database &database, std::string_view sql);
	~Statement();
	Statement(const Statement &) = delete;
	Statement &operator=(const Statement &) = delete;
	Statement(Statement &&other) noexcept;
	Statement &operator=(Statement &&other) noexcept;

	void bind(int index, std::int64_t value);
	void bind(int index, std::uint64_t value);
	void bind(int index, std::int32_t value);
	void bind(int index, std::string_view value);
	void bind(int index, std::span<const std::byte> value);
	void bind_null(int index);
	[[nodiscard]] bool step();
	void execute();
	void reset();

	[[nodiscard]] std::int64_t integer(int column) const;
	[[nodiscard]] std::string text(int column) const;
	[[nodiscard]] std::vector<std::byte> blob(int column) const;

private:
	void check(int result, std::string_view operation) const;
	Database *database_ = nullptr;
	sqlite3_stmt *statement_ = nullptr;
};

/** Small RAII boundary over the SQLite C API. */
class Database final {
public:
	explicit Database(const std::filesystem::path &path);
	~Database();
	Database(const Database &) = delete;
	Database &operator=(const Database &) = delete;
	Database(Database &&other) noexcept;
	Database &operator=(Database &&other) noexcept;

	void execute(std::string_view sql);
	[[nodiscard]] Statement prepare(std::string_view sql);
	[[nodiscard]] std::int64_t last_insert_rowid() const;
	[[nodiscard]] int changes() const;
	[[nodiscard]] sqlite3 *native_handle() const noexcept { return handle_; }
	[[nodiscard]] std::string error_message() const;

private:
	sqlite3 *handle_ = nullptr;
};

class Transaction final {
public:
	explicit Transaction(Database &database);
	~Transaction();
	Transaction(const Transaction &) = delete;
	Transaction &operator=(const Transaction &) = delete;
	void commit();

private:
	Database &database_;
	bool committed_ = false;
};

} // namespace mnc::storage::sqlite

