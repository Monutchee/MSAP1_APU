#pragma once

#include <cstdint>
#include <source_location>
#include <span>
#include <string>
#include <string_view>

namespace mnc::logging {

enum class Priority : std::uint8_t {
	emergency = 0,
	alert = 1,
	critical = 2,
	error = 3,
	warning = 4,
	notice = 5,
	info = 6,
	debug = 7,
};

struct Field {
	std::string name;
	std::string value;
};

const char *priority_name(Priority priority) noexcept;
bool parse_priority(std::string_view name, Priority &priority) noexcept;
bool valid_field_name(std::string_view name) noexcept;

/*
 * A best-effort structured journal writer.
 *
 * MESSAGE remains human-readable while MNC_COMPONENT, MNC_MODULE and
 * MNC_EVENT provide stable machine-readable classification. write() never
 * throws: acquisition and control paths must continue even when journald is
 * unavailable.
 */
class Logger {
public:
	explicit Logger(std::string component, std::string module = {});

	bool write(Priority priority, std::string_view message,
		   std::string_view event = {},
		   std::span<const Field> fields = {},
		   const std::source_location &source =
			   std::source_location::current()) const noexcept;

	const std::string &component() const noexcept { return component_; }
	const std::string &module() const noexcept { return module_; }

private:
	std::string component_;
	std::string module_;
};

} // namespace mnc::logging
