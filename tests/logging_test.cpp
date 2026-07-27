#include "mnc/logging/journal_reader.hpp"
#include "mnc/logging/logging.hpp"
#include "journal_query.hpp"

#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

void priorities()
{
	mnc::logging::Priority priority{};
	require(mnc::logging::parse_priority("warning", priority) &&
			priority == mnc::logging::Priority::warning,
		"warning priority was not parsed");
	require(mnc::logging::parse_priority("ERR", priority) &&
			priority == mnc::logging::Priority::error,
		"priority aliases are not case insensitive");
	require(!mnc::logging::parse_priority("verbose", priority),
		"unknown priority was accepted");
	require(std::string(mnc::logging::priority_name(
			mnc::logging::Priority::notice)) == "notice",
		"priority name is incorrect");
}

void fields()
{
	require(mnc::logging::valid_field_name("MNC_REQUEST_ID"),
		"valid journal field was rejected");
	require(!mnc::logging::valid_field_name("_PRIVATE"),
		"reserved journal field was accepted");
	require(!mnc::logging::valid_field_name("lowercase"),
		"lowercase journal field was accepted");
	require(!mnc::logging::valid_field_name("BAD-FIELD"),
		"punctuated journal field was accepted");
}

void json()
{
	mnc::logging::Entry entry;
	entry.timestamp = std::chrono::system_clock::time_point{
		std::chrono::microseconds{1234}};
	entry.cursor.value = "s=cursor";
	entry.priority = mnc::logging::Priority::warning;
	entry.message = "line one\n\"line two\"";
	entry.component = "fpga-acquisition";
	entry.module = "dma";
	entry.event = "dma_error";
	entry.configuration_generation = "0x1234";
	entry.unit = "msap1-fpga-acquisition.service";
	entry.source_file = "acquisition.cpp";
	entry.source_line = "42";
	entry.source_function = "run";
	const auto output = mnc::logging::entry_to_json(entry);
	require(output.find("\"timestamp_usec\":1234") != std::string::npos,
		"JSON omitted timestamp");
	require(output.find("\"priority\":\"warning\"") != std::string::npos,
		"JSON omitted priority");
	require(output.find("line one\\n\\\"line two\\\"") != std::string::npos,
		"JSON did not escape the message");
	require(output.find("\"component\":\"fpga-acquisition\"") !=
			std::string::npos,
		"JSON omitted component");
	require(output.find("\"source_file\":\"acquisition.cpp\"") !=
			std::string::npos &&
			output.find("\"source_line\":\"42\"") !=
				std::string::npos &&
			output.find("\"source_function\":\"run\"") !=
				std::string::npos,
		"JSON omitted source metadata");
}

void writer_failure_is_safe()
{
	const mnc::logging::Logger logger("test", "unit");
	const mnc::logging::Field fields[]{{"BAD-FIELD", "ignored"}};
	// On host builds without libsystemd this returns false; on target builds it
	// may return true. Either way it must never throw.
	(void)logger.write(mnc::logging::Priority::info, "test message",
			   "test_event", fields);
}

void bounded_query_and_cursor_continuation()
{
	using namespace std::chrono_literals;
	std::array<mnc::logging::Entry, 5> entries;
	for (std::size_t index = 0; index < entries.size(); ++index) {
		entries[index].timestamp =
			std::chrono::system_clock::time_point{1s + 1ms * index};
		entries[index].cursor.value = "cursor-" + std::to_string(index);
		entries[index].component =
			index == 3 ? std::string{} : "fpga-acquisition";
		entries[index].module = index == 1 ? "rpmsg" : "dma";
		entries[index].priority = index == 4
			? mnc::logging::Priority::error
			: mnc::logging::Priority::info;
	}

	mnc::logging::Query query;
	query.component = "fpga-acquisition";
	query.module = "dma";
	query.limit = 2;
	auto page = mnc::logging::detail::bounded_page(entries, query);
	require(page.size() == 2 && page[0].cursor.value == "cursor-0" &&
			page[1].cursor.value == "cursor-2",
		"bounded query did not apply classification and limit");

	query.after = page.back().cursor;
	page = mnc::logging::detail::bounded_page(entries, query);
	require(page.size() == 1 && page.front().cursor.value == "cursor-4",
		"cursor continuation repeated or skipped an entry");

	query.maximum_priority = mnc::logging::Priority::warning;
	query.after.reset();
	page = mnc::logging::detail::bounded_page(entries, query);
	require(page.size() == 1 && page.front().priority ==
			mnc::logging::Priority::error,
		"priority threshold did not retain only severe entries");

	query.component.reset();
	query.module.reset();
	query.maximum_priority.reset();
	query.components = {"firmware"};
	query.units = {"msap1-fpga-acquisition.service"};
	entries[3].unit = "msap1-fpga-acquisition.service";
	page = mnc::logging::detail::bounded_page(entries, query);
	require(page.size() == 1 && page.front().cursor.value == "cursor-3",
		"unit fallback did not recover a malformed unclassified entry");
}

} // namespace

int main()
{
	try {
		priorities();
		fields();
		json();
		writer_failure_is_safe();
		bounded_query_and_cursor_continuation();
		std::cout << "logging tests passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "logging test failed: " << error.what() << '\n';
		return 1;
	}
}
