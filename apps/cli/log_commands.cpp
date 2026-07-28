#include "cli.hpp"
#include "result_output.hpp"

#include "mnc/logging/journal_reader.hpp"
#include "mnc/logging/logging.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace msap1::cli {
namespace {

std::uint64_t parse_positive_integer(const std::string &value,
				     const std::string &option)
{
	std::size_t end = 0;
	std::uint64_t result = 0;
	try {
		result = std::stoull(value, &end, 0);
	} catch (const std::exception &) {
		throw std::invalid_argument(option + " requires a positive integer");
	}
	if (end != value.size() || result == 0)
		throw std::invalid_argument(option + " requires a positive integer");
	return result;
}

std::chrono::system_clock::time_point parse_since(const std::string &value)
{
	std::istringstream input(value);
	std::uint64_t count = 0;
	std::string unit;
	std::string suffix;
	if (!(input >> count >> unit) || count == 0)
		throw std::invalid_argument(
			"--since must look like '10 minutes ago'");
	(void)(input >> suffix);
	if (!suffix.empty() && suffix != "ago")
		throw std::invalid_argument(
			"--since must look like '10 minutes ago'");
	std::string trailing;
	if (input >> trailing)
		throw std::invalid_argument(
			"--since must look like '10 minutes ago'");
	std::transform(unit.begin(), unit.end(), unit.begin(),
		[](unsigned char character) {
			return static_cast<char>(std::tolower(character));
		});
	std::chrono::seconds duration;
	if (unit == "second" || unit == "seconds")
		duration = std::chrono::seconds{count};
	else if (unit == "minute" || unit == "minutes")
		duration = std::chrono::minutes{count};
	else if (unit == "hour" || unit == "hours")
		duration = std::chrono::hours{count};
	else if (unit == "day" || unit == "days")
		duration = std::chrono::hours{count * 24u};
	else
		throw std::invalid_argument(
			"--since supports seconds, minutes, hours, or days");
	return std::chrono::system_clock::now() - duration;
}

std::pair<std::string, std::string>
classification(const mnc::logging::Entry &entry)
{
	if (!entry.component.empty())
		return {entry.component, entry.module};
	if (entry.unit == "dfx-mgr-fw-load.service")
		return {"firmware", "pl"};
	if (entry.unit == "msap1-dfx-firmware-rpu-load.service")
		return {"firmware", "rpu"};
	if (entry.unit == "msap1-fpga-acquisition.service")
		return {"fpga-acquisition", {}};
	if (entry.unit == "msap1-web-backend.service")
		return {"web-backend", {}};
	return {{}, {}};
}

std::string base64url_encode(std::string_view input)
{
	static constexpr std::string_view alphabet =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
	std::string output;
	output.reserve((input.size() * 4 + 2) / 3);
	std::uint32_t accumulator = 0;
	unsigned bits = 0;
	for (const auto byte : input) {
		accumulator = (accumulator << 8) |
			static_cast<unsigned char>(byte);
		bits += 8;
		while (bits >= 6) {
			bits -= 6;
			output.push_back(alphabet[(accumulator >> bits) & 0x3fu]);
		}
	}
	if (bits != 0)
		output.push_back(alphabet[(accumulator << (6 - bits)) & 0x3fu]);
	return output;
}

std::string base64url_decode(std::string_view input)
{
	auto value = [](char character) -> int {
		if (character >= 'A' && character <= 'Z')
			return character - 'A';
		if (character >= 'a' && character <= 'z')
			return character - 'a' + 26;
		if (character >= '0' && character <= '9')
			return character - '0' + 52;
		if (character == '-')
			return 62;
		if (character == '_')
			return 63;
		return -1;
	};
	std::string output;
	std::uint32_t accumulator = 0;
	unsigned bits = 0;
	for (const auto character : input) {
		const auto decoded = value(character);
		if (decoded < 0)
			throw std::invalid_argument("--cursor is not valid base64url");
		accumulator = (accumulator << 6) |
			static_cast<std::uint32_t>(decoded);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			output.push_back(static_cast<char>(
				(accumulator >> bits) & 0xffu));
		}
	}
	return output;
}

struct LogEntryDto {
	std::int64_t timestamp_unix_ms = 0;
	std::string priority;
	std::string component;
	std::string module;
	std::string event;
	std::string message;
	std::string request_id;
	std::string configuration_generation;
	std::string unit;
	std::string executable;
	std::string source_file;
	std::string source_line;
	std::string source_function;
};

struct LogPageDto {
	std::vector<LogEntryDto> entries;
	std::string next_cursor;
	bool has_more = false;
};

LogEntryDto log_entry_dto(const mnc::logging::Entry &entry)
{
	const auto [fallback_component, fallback_module] =
		classification(entry);
	return {
		.timestamp_unix_ms =
			std::chrono::duration_cast<std::chrono::milliseconds>(
				entry.timestamp.time_since_epoch())
				.count(),
		.priority = std::string(
			mnc::logging::priority_name(entry.priority)),
		.component = entry.component.empty() ? fallback_component
						    : entry.component,
		.module = entry.module.empty() ? fallback_module : entry.module,
		.event = entry.event,
		.message = entry.message,
		.request_id = entry.request_id,
		.configuration_generation =
			entry.configuration_generation,
		.unit = entry.unit,
		.executable = entry.executable,
		.source_file = entry.source_file,
		.source_line = entry.source_line,
		.source_function = entry.source_function,
	};
}

void print_entry(std::ostream &output, const mnc::logging::Entry &entry,
		 bool json)
{
	if (json) {
		auto classified = entry;
		const auto [component, module] = classification(entry);
		if (classified.component.empty())
			classified.component = component;
		if (classified.module.empty())
			classified.module = module;
		output << mnc::logging::entry_to_json(classified) << '\n';
		return;
	}
	const auto time = std::chrono::system_clock::to_time_t(entry.timestamp);
	std::tm calendar{};
	localtime_r(&time, &calendar);
	const auto milliseconds =
		std::chrono::duration_cast<std::chrono::milliseconds>(
			entry.timestamp.time_since_epoch()) %
		std::chrono::seconds{1};
	const auto [component, module] = classification(entry);
	output << std::put_time(&calendar, "%Y-%m-%d %H:%M:%S") << '.'
	       << std::setw(3) << std::setfill('0') << milliseconds.count()
	       << ' ' << std::left << std::setw(7) << std::setfill(' ')
	       << mnc::logging::priority_name(entry.priority) << ' '
	       << (component.empty() ? "unknown" : component);
	if (!module.empty())
		output << '/' << module;
	if (!entry.event.empty())
		output << " [" << entry.event << ']';
	output << ": " << entry.message << '\n';
}

int show_logs(const Options &options, std::ostream &output)
{
	mnc::logging::Query query;
	query.limit = options.result_limit.value_or(100);
	query.component = options.log_component;
	query.module = options.log_module;
	query.components = {"fpga-acquisition", "web-backend", "firmware"};
	query.units = {
		"msap1-fpga-acquisition.service",
		"msap1-web-backend.service",
		"dfx-mgr-fw-load.service",
		"msap1-dfx-firmware-rpu-load.service",
	};
	if (options.log_component == "firmware") {
		query.component.reset();
		query.components = {"firmware"};
		query.units = {
			"dfx-mgr-fw-load.service",
			"msap1-dfx-firmware-rpu-load.service",
		};
	} else if (options.log_component) {
		query.components.clear();
		query.units.clear();
	}
	if (options.log_priority) {
		mnc::logging::Priority priority;
		if (!mnc::logging::parse_priority(*options.log_priority, priority))
			throw std::invalid_argument("unsupported log priority");
		query.maximum_priority = priority;
	}
	if (options.log_since)
		query.since = parse_since(*options.log_since);
	if (options.log_cursor)
		query.after =
			mnc::logging::Cursor{base64url_decode(*options.log_cursor)};

	mnc::logging::JournalReader reader;
	if (options.output_format == OutputFormat::json) {
		const auto requested_limit = query.limit;
		query.limit = requested_limit + 1;
		auto entries = reader.read(query);
		const bool has_more = entries.size() > requested_limit;
		if (has_more)
			entries.resize(requested_limit);
		LogPageDto page;
		page.has_more = has_more;
		for (const auto &entry : entries)
			page.entries.push_back(log_entry_dto(entry));
		if (has_more && !entries.empty())
			page.next_cursor =
				base64url_encode(entries.back().cursor.value);
		write_json_success(output, page);
		return 0;
	}
	auto print = [&](const mnc::logging::Entry &entry) {
		print_entry(output, entry, options.log_json);
		output.flush();
		return !stop_was_requested();
	};
	if (options.log_follow) {
		reader.follow(query, print, std::chrono::milliseconds{250},
			      [] { return !stop_was_requested(); });
		return 0;
	}
	for (const auto &entry : reader.read(query))
		(void)print(entry);
	return 0;
}

} // namespace

void register_log_command(Application &application)
{
	Command command(
		"log", "View consolidated structured MSAP1 system logs",
		show_logs,
		{
			.access = AccessLevel::diagnostic,
			.side_effect = SideEffect::none,
			.supports_text = true,
			.supports_json = true,
			.variants = {
				{"--follow", AccessLevel::local_only,
				 "Continuously follow journal entries"},
			},
		});
	command.add_option({
		"component", "NAME", "Filter by process component",
		CompletionKind::none,
		[](Options &options, const std::string &value) {
			options.log_component = value;
		},
	});
	command.add_option({
		"cursor", "TOKEN", "Continue a bounded machine-readable query",
		CompletionKind::none,
		[](Options &options, const std::string &value) {
			(void)base64url_decode(value);
			options.log_cursor = value;
		},
	});
	command.add_option({
		"module", "NAME", "Filter by internal module",
		CompletionKind::none,
		[](Options &options, const std::string &value) {
			options.log_module = value;
		},
	});
	command.add_option({
		"priority", "LEVEL", "Show LEVEL and more severe entries",
		CompletionKind::none,
		[](Options &options, const std::string &value) {
			mnc::logging::Priority priority;
			if (!mnc::logging::parse_priority(value, priority))
				throw std::invalid_argument(
					"--priority must be emergency, alert, critical, "
					"error, warning, notice, info, or debug");
			options.log_priority = value;
		},
	});
	command.add_option({
		"since", "WHEN", "Show entries since e.g. '10 minutes ago'",
		CompletionKind::none,
		[](Options &options, const std::string &value) {
			(void)parse_since(value);
			options.log_since = value;
		},
	});
	command.add_option({
		"limit", "COUNT", "Maximum initial entries (default: 100)",
		CompletionKind::none,
		[](Options &options, const std::string &value) {
			const auto limit = parse_positive_integer(value, "--limit");
			if (limit > 10000)
				throw std::invalid_argument(
					"--limit must not exceed 10000");
			options.result_limit = limit;
		},
	});
	command.add_option({
		"follow", "", "Follow new entries until interrupted",
		CompletionKind::none,
		[](Options &options, const std::string &) {
			options.log_follow = true;
		},
		false,
	});
	command.add_option({
		"json", "", "Write one JSON object per entry",
		CompletionKind::none,
		[](Options &options, const std::string &) {
			options.log_json = true;
		},
		false,
	});
	command.set_access_resolver([](const Options &options) {
		return options.log_follow ? AccessLevel::local_only
					  : AccessLevel::diagnostic;
	});
	application.add_command(std::move(command));
}

} // namespace msap1::cli
