#pragma once

#include "mnc/logging/logging.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mnc::logging {

struct Cursor {
	std::string value;

	explicit operator bool() const noexcept { return !value.empty(); }
};

struct Entry {
	std::chrono::system_clock::time_point timestamp;
	Cursor cursor;
	Priority priority = Priority::info;
	std::string message;
	std::string component;
	std::string module;
	std::string event;
	std::string request_id;
	std::string configuration_generation;
	std::string unit;
	std::string executable;
	std::string source_file;
	std::string source_line;
	std::string source_function;
};

struct Query {
	std::optional<std::string> component;
	std::optional<std::string> module;
	std::optional<Priority> maximum_priority;
	std::optional<std::chrono::system_clock::time_point> since;
	std::optional<Cursor> after;
	std::vector<std::string> components;
	std::vector<std::string> units;
	std::size_t limit = 100;
};

/*
 * Reader over the system journal. Cursor-based continuation is deliberately
 * part of the public API so a future MCP server can expose bounded, stable
 * pagination without relying on timestamp equality.
 */
class JournalReader {
public:
	using FollowHandler = std::function<bool(const Entry &)>;
	using ContinueHandler = std::function<bool()>;

	JournalReader();
	~JournalReader();
	JournalReader(JournalReader &&) noexcept;
	JournalReader &operator=(JournalReader &&) noexcept;
	JournalReader(const JournalReader &) = delete;
	JournalReader &operator=(const JournalReader &) = delete;

	bool available() const noexcept;
	std::vector<Entry> read(const Query &query);
	void follow(const Query &query, const FollowHandler &handler,
		    std::chrono::milliseconds wake_interval =
			    std::chrono::milliseconds{250},
		    const ContinueHandler &keep_running = {});

private:
	class Implementation;
	std::unique_ptr<Implementation> implementation_;
};

std::string entry_to_json(const Entry &entry);

} // namespace mnc::logging
