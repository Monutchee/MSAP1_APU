#include "mnc/logging/journal_reader.hpp"
#include "journal_query.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string_view>

#if defined(MNC_LOGGING_HAVE_SYSTEMD)
#include <systemd/sd-journal.h>
#endif

namespace mnc::logging {
namespace {

std::string json_escape(std::string_view value)
{
	std::ostringstream output;
	for (const unsigned char character : value) {
		switch (character) {
		case '"': output << "\\\""; break;
		case '\\': output << "\\\\"; break;
		case '\b': output << "\\b"; break;
		case '\f': output << "\\f"; break;
		case '\n': output << "\\n"; break;
		case '\r': output << "\\r"; break;
		case '\t': output << "\\t"; break;
		default:
			if (character < 0x20)
				output << "\\u" << std::hex << std::setw(4)
				       << std::setfill('0')
				       << static_cast<unsigned>(character);
			else
				output << static_cast<char>(character);
		}
	}
	return output.str();
}

bool listed(const std::vector<std::string> &values, const std::string &value)
{
	return values.empty() ||
		std::find(values.begin(), values.end(), value) != values.end();
}

#if defined(MNC_LOGGING_HAVE_SYSTEMD)
std::string field(sd_journal *journal, const char *name)
{
	const void *data = nullptr;
	std::size_t size = 0;
	if (sd_journal_get_data(journal, name, &data, &size) < 0 ||
	    data == nullptr)
		return {};
	const std::string_view value(static_cast<const char *>(data), size);
	const auto separator = value.find('=');
	if (separator == std::string_view::npos)
		return {};
	return std::string(value.substr(separator + 1));
}

Entry current_entry(sd_journal *journal)
{
	Entry result;
	std::uint64_t timestamp = 0;
	if (sd_journal_get_realtime_usec(journal, &timestamp) >= 0)
		result.timestamp = std::chrono::system_clock::time_point{
			std::chrono::microseconds{timestamp}};
	char *cursor = nullptr;
	if (sd_journal_get_cursor(journal, &cursor) >= 0 && cursor != nullptr) {
		result.cursor.value = cursor;
		std::free(cursor);
	}
	result.message = field(journal, "MESSAGE");
	result.component = field(journal, "MNC_COMPONENT");
	result.module = field(journal, "MNC_MODULE");
	result.event = field(journal, "MNC_EVENT");
	result.request_id = field(journal, "MNC_REQUEST_ID");
	result.configuration_generation =
		field(journal, "MNC_CONFIGURATION_GENERATION");
	/*
	 * PID 1 lifecycle messages carry the affected service in UNIT while
	 * _SYSTEMD_UNIT identifies init.scope. Service stdout/stderr normally
	 * has only _SYSTEMD_UNIT, so prefer UNIT when it is present.
	 */
	result.unit = field(journal, "UNIT");
	if (result.unit.empty())
		result.unit = field(journal, "_SYSTEMD_UNIT");
	result.executable = field(journal, "_EXE");
	result.source_file = field(journal, "CODE_FILE");
	result.source_line = field(journal, "CODE_LINE");
	result.source_function = field(journal, "CODE_FUNC");

	const auto priority = field(journal, "PRIORITY");
	unsigned numeric = static_cast<unsigned>(Priority::info);
	const auto parsed = std::from_chars(
		priority.data(), priority.data() + priority.size(), numeric);
	if (parsed.ec == std::errc{} &&
	    numeric <= static_cast<unsigned>(Priority::debug))
		result.priority = static_cast<Priority>(numeric);
	return result;
}
#endif

} // namespace

namespace detail {

bool matches_query(const Entry &entry, const Query &query) noexcept
{
	if (query.component && entry.component != *query.component)
		return false;
	if (query.module && entry.module != *query.module)
		return false;
	if (query.maximum_priority &&
	    static_cast<unsigned>(entry.priority) >
		    static_cast<unsigned>(*query.maximum_priority))
		return false;
	const bool component_match =
		listed(query.components, entry.component);
	const bool unit_match = listed(query.units, entry.unit);
	if (!query.components.empty() && !query.units.empty())
		return component_match || unit_match;
	return component_match && unit_match;
}

std::vector<Entry> bounded_page(std::span<const Entry> entries,
				const Query &query)
{
	std::vector<Entry> result;
	if (query.limit == 0)
		return result;
	result.reserve(std::min(query.limit, entries.size()));

	bool after_cursor = !query.after || !*query.after;
	for (const auto &entry : entries) {
		if (!after_cursor) {
			if (entry.cursor.value == query.after->value)
				after_cursor = true;
			continue;
		}
		if (query.since && entry.timestamp < *query.since)
			continue;
		if (!matches_query(entry, query))
			continue;
		result.push_back(entry);
		if (result.size() == query.limit)
			break;
	}
	return result;
}

} // namespace detail

class JournalReader::Implementation {
public:
#if defined(MNC_LOGGING_HAVE_SYSTEMD)
	Implementation()
	{
		if (sd_journal_open(&journal_, SD_JOURNAL_LOCAL_ONLY) < 0)
			journal_ = nullptr;
	}
	~Implementation()
	{
		if (journal_ != nullptr)
			sd_journal_close(journal_);
	}

	sd_journal *journal_ = nullptr;
#else
	Implementation() = default;
#endif
};

JournalReader::JournalReader()
	: implementation_(std::make_unique<Implementation>())
{
}

JournalReader::~JournalReader() = default;
JournalReader::JournalReader(JournalReader &&) noexcept = default;
JournalReader &JournalReader::operator=(JournalReader &&) noexcept = default;

bool JournalReader::available() const noexcept
{
#if defined(MNC_LOGGING_HAVE_SYSTEMD)
	return implementation_ && implementation_->journal_ != nullptr;
#else
	return false;
#endif
}

std::vector<Entry> JournalReader::read(const Query &query)
{
	if (query.limit == 0)
		return {};
#if !defined(MNC_LOGGING_HAVE_SYSTEMD)
	(void)query;
	throw std::runtime_error("journald support is unavailable in this build");
#else
	if (!available())
		throw std::runtime_error("failed to open the system journal");
	auto *journal = implementation_->journal_;
	std::vector<Entry> result;
	result.reserve(query.limit);

	if (query.after && *query.after) {
		if (sd_journal_seek_cursor(
			    journal, query.after->value.c_str()) < 0)
			throw std::runtime_error("journal cursor is no longer valid");
		bool skipped_cursor = false;
		while (result.size() < query.limit &&
		       sd_journal_next(journal) > 0) {
			auto entry = current_entry(journal);
			if (!skipped_cursor && entry.cursor.value ==
						      query.after->value) {
				skipped_cursor = true;
				continue;
			}
			skipped_cursor = true;
			if (detail::matches_query(entry, query))
				result.push_back(std::move(entry));
		}
		return result;
	}

	if (query.since) {
		const auto usec = std::chrono::duration_cast<std::chrono::microseconds>(
			query.since->time_since_epoch());
		if (sd_journal_seek_realtime_usec(journal, usec.count()) < 0)
			throw std::runtime_error("failed to seek the system journal");
		while (result.size() < query.limit &&
		       sd_journal_next(journal) > 0) {
			auto entry = current_entry(journal);
			if (detail::matches_query(entry, query))
				result.push_back(std::move(entry));
		}
		return result;
	}

	if (sd_journal_seek_tail(journal) < 0)
		throw std::runtime_error("failed to seek the system journal");
	while (result.size() < query.limit && sd_journal_previous(journal) > 0) {
		auto entry = current_entry(journal);
		if (detail::matches_query(entry, query))
			result.push_back(std::move(entry));
	}
	std::reverse(result.begin(), result.end());
	return result;
#endif
}

void JournalReader::follow(const Query &query, const FollowHandler &handler,
			   std::chrono::milliseconds wake_interval,
			   const ContinueHandler &keep_running)
{
#if !defined(MNC_LOGGING_HAVE_SYSTEMD)
	(void)query;
	(void)handler;
	(void)wake_interval;
	(void)keep_running;
	throw std::runtime_error("journald support is unavailable in this build");
#else
	if (!available())
		throw std::runtime_error("failed to open the system journal");
	Query continuation = query;
	auto entries = read(continuation);
	for (const auto &entry : entries) {
		if (!handler(entry))
			return;
		continuation.after = entry.cursor;
		continuation.since =
			entry.timestamp + std::chrono::microseconds{1};
	}
	if (!continuation.after) {
		if (sd_journal_seek_tail(implementation_->journal_) < 0)
			throw std::runtime_error("failed to follow the system journal");
	}
	for (;;) {
		if (keep_running && !keep_running())
			return;
		/*
		 * Drain an initial --since backlog in bounded pages before waiting.
		 * Once the cursor reaches the tail, read() returns an empty page and
		 * the journal wait below becomes the only blocking operation.
		 */
		continuation.limit = query.limit;
		entries = read(continuation);
		if (!entries.empty()) {
			for (const auto &entry : entries) {
				if (!handler(entry))
					return;
				continuation.after = entry.cursor;
				continuation.since =
					entry.timestamp +
					std::chrono::microseconds{1};
			}
			continue;
		}

		const auto timeout = static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::microseconds>(
				wake_interval)
				.count());
		const int state = sd_journal_wait(
			implementation_->journal_, timeout);
		if (state < 0)
			throw std::runtime_error("failed while following the journal");
		if (keep_running && !keep_running())
			return;
		if (state == SD_JOURNAL_INVALIDATE && continuation.after) {
			if (sd_journal_seek_cursor(
				    implementation_->journal_,
				    continuation.after->value.c_str()) < 0)
				continuation.after.reset();
		}
	}
#endif
}

std::string entry_to_json(const Entry &entry)
{
	const auto usec = std::chrono::duration_cast<std::chrono::microseconds>(
		entry.timestamp.time_since_epoch());
	std::ostringstream output;
	output << "{\"timestamp_usec\":" << usec.count()
	       << ",\"cursor\":\"" << json_escape(entry.cursor.value)
	       << "\",\"priority\":\"" << priority_name(entry.priority)
	       << "\",\"message\":\"" << json_escape(entry.message)
	       << "\",\"component\":\"" << json_escape(entry.component)
	       << "\",\"module\":\"" << json_escape(entry.module)
	       << "\",\"event\":\"" << json_escape(entry.event)
	       << "\",\"request_id\":\"" << json_escape(entry.request_id)
	       << "\",\"configuration_generation\":\""
	       << json_escape(entry.configuration_generation)
	       << "\",\"unit\":\"" << json_escape(entry.unit)
	       << "\",\"executable\":\"" << json_escape(entry.executable)
	       << "\",\"source_file\":\"" << json_escape(entry.source_file)
	       << "\",\"source_line\":\"" << json_escape(entry.source_line)
	       << "\",\"source_function\":\""
	       << json_escape(entry.source_function) << "\"}";
	return output.str();
}

} // namespace mnc::logging
