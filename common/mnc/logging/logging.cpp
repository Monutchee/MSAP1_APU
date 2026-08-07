#include "mnc/logging/logging.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <sys/uio.h>
#include <vector>

#if defined(MNC_LOGGING_HAVE_SYSTEMD)
#include <systemd/sd-journal.h>
#endif

namespace mnc::logging {
namespace {

std::string journal_field(std::string_view name, std::string_view value)
{
	std::string result;
	result.reserve(name.size() + value.size() + 1);
	result.append(name);
	result.push_back('=');
	result.append(value);
	return result;
}

} // namespace

const char *priority_name(Priority priority) noexcept
{
	static constexpr std::array<const char *, 8> names{
		"emergency", "alert", "critical", "error",
		"warning", "notice", "info", "debug"};
	const auto index = static_cast<std::size_t>(priority);
	return index < names.size() ? names[index] : "info";
}

bool parse_priority(std::string_view name, Priority &priority) noexcept
{
	std::string normalized(name);
	std::transform(normalized.begin(), normalized.end(), normalized.begin(),
		[](unsigned char character) {
			return static_cast<char>(std::tolower(character));
		});
	static constexpr std::array<std::pair<std::string_view, Priority>, 10>
		values{{
			{"emergency", Priority::emergency},
			{"emerg", Priority::emergency},
			{"alert", Priority::alert},
			{"critical", Priority::critical},
			{"crit", Priority::critical},
			{"error", Priority::error},
			{"err", Priority::error},
			{"warning", Priority::warning},
			{"notice", Priority::notice},
			{"info", Priority::info},
		}};
	for (const auto &[candidate, value] : values) {
		if (normalized == candidate) {
			priority = value;
			return true;
		}
	}
	if (normalized == "debug") {
		priority = Priority::debug;
		return true;
	}
	return false;
}

bool valid_field_name(std::string_view name) noexcept
{
	if (name.empty() || name.front() == '_')
		return false;
	return std::all_of(name.begin(), name.end(), [](unsigned char character) {
		return (character >= 'A' && character <= 'Z') ||
			(character >= '0' && character <= '9') ||
			character == '_';
	});
}

Logger::Logger(std::string component, std::string module)
	: component_(std::move(component)), module_(std::move(module))
{
	if (component_.empty())
		throw std::invalid_argument("logging component must not be empty");
}

bool Logger::write(Priority priority, std::string_view message,
		   std::string_view event, std::span<const Field> fields,
		   const std::source_location &source) const noexcept
{
	try {
		std::vector<std::string> values;
		values.reserve(10 + fields.size());
		values.push_back(journal_field("MESSAGE", message));
		values.push_back(journal_field(
			"PRIORITY",
			std::to_string(static_cast<unsigned>(priority))));
		values.push_back(journal_field("MNC_COMPONENT", component_));
		if (!module_.empty())
			values.push_back(journal_field("MNC_MODULE", module_));
		if (!event.empty())
			values.push_back(journal_field("MNC_EVENT", event));
		values.push_back(journal_field("CODE_FILE", source.file_name()));
		values.push_back(journal_field(
			"CODE_LINE", std::to_string(source.line())));
		values.push_back(journal_field("CODE_FUNC", source.function_name()));
		for (const auto &field : fields) {
			if (valid_field_name(field.name))
				values.push_back(journal_field(field.name, field.value));
		}

#if defined(MNC_LOGGING_HAVE_SYSTEMD)
		std::vector<iovec> vectors;
		vectors.reserve(values.size());
		for (auto &value : values)
			vectors.push_back(
				{value.data(), static_cast<std::size_t>(value.size())});
		return sd_journal_sendv(vectors.data(),
				       static_cast<int>(vectors.size())) >= 0;
#else
		(void)values;
		return false;
#endif
	} catch (...) {
		return false;
	}
}

} // namespace mnc::logging
