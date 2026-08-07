/**
 * @file developer_routes.cpp
 * @brief Administrator-only diagnostics: SoC temperatures, component
 *        fingerprints, and the bounded journald query.
 */

#include "response.hpp"
#include "routes.hpp"

#include "msap1/system/soc_temperature.hpp"
#include "msap1/system/system_identity.hpp"
#include "mnc/logging/journal_reader.hpp"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace msap1::web::api {

namespace {

/** One journal entry in GET /api/v1/developer/logs. */
struct DeveloperLogEntryDto {
	std::int64_t timestamp_usec;
	std::string cursor;
	std::string priority;
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
	std::string raw;
};

/** Body of GET /api/v1/developer/logs. */
struct DeveloperLogsDto {
	std::vector<DeveloperLogEntryDto> entries;
	std::string next_cursor;
};

/** One sensor in GET /api/v1/developer/temperatures. */
struct SocTemperatureDto {
	std::string zone;
	std::string label;
	bool available;
	std::int64_t millidegrees_c;
	double temperature_c;
};

/** Body of GET /api/v1/developer/temperatures. */
struct SocTemperaturesDto {
	std::int64_t sampled_at_unix_ms;
	std::vector<SocTemperatureDto> sensors;
};

/** One component in GET /api/v1/developer/about. */
struct ComponentFingerprintDto {
	std::string id;
	std::string label;
	std::string component_type;
	std::string path;
	bool available;
	std::uintmax_t size_bytes;
	std::string md5;
};

/** Body of GET /api/v1/developer/about. */
struct DeveloperAboutDto {
	std::string digest_algorithm;
	std::string digest_purpose;
	std::vector<ComponentFingerprintDto> components;
};

unsigned hex_digit(char value)
{
	if (value >= '0' && value <= '9')
		return static_cast<unsigned>(value - '0');
	if (value >= 'a' && value <= 'f')
		return static_cast<unsigned>(value - 'a' + 10);
	if (value >= 'A' && value <= 'F')
		return static_cast<unsigned>(value - 'A' + 10);
	throw std::invalid_argument("invalid URL encoding");
}

std::string url_decode(std::string_view value)
{
	std::string result;
	result.reserve(value.size());
	for (std::size_t index = 0; index < value.size(); ++index) {
		if (value[index] == '+') {
			result.push_back(' ');
			continue;
		}
		if (value[index] != '%') {
			result.push_back(value[index]);
			continue;
		}
		if (index + 2 >= value.size())
			throw std::invalid_argument("invalid URL encoding");
		const auto byte = (hex_digit(value[index + 1]) << 4u) |
				  hex_digit(value[index + 2]);
		result.push_back(static_cast<char>(byte));
		index += 2;
	}
	return result;
}

/** Parse the query string of @p target into decoded name/value pairs. */
std::unordered_map<std::string, std::string>
query_parameters(std::string_view target)
{
	std::unordered_map<std::string, std::string> result;
	const auto question = target.find('?');
	if (question == std::string_view::npos)
		return result;
	auto query = target.substr(question + 1);
	while (!query.empty()) {
		const auto separator = query.find('&');
		const auto item = query.substr(0, separator);
		const auto equals = item.find('=');
		const auto name = url_decode(item.substr(0, equals));
		const auto value = equals == std::string_view::npos
			? std::string{}
			: url_decode(item.substr(equals + 1));
		if (!name.empty())
			result.insert_or_assign(name, value);
		if (separator == std::string_view::npos)
			break;
		query.remove_prefix(separator + 1);
	}
	return result;
}

/** @throws std::invalid_argument unless 1 <= limit <= 500 (default 100). */
std::size_t
log_limit(const std::unordered_map<std::string, std::string> &params)
{
	const auto item = params.find("limit");
	if (item == params.end())
		return 100;
	std::size_t value = 0;
	const auto parsed = std::from_chars(
		item->second.data(),
		item->second.data() + item->second.size(), value);
	if (parsed.ec != std::errc{} ||
	    parsed.ptr != item->second.data() + item->second.size() ||
	    value == 0 || value > 500)
		throw std::invalid_argument(
			"log limit must be between 1 and 500");
	return value;
}

/** Derive component/module for entries that journald attributes to a unit. */
std::pair<std::string, std::string>
classify_log_entry(const mnc::logging::Entry &entry)
{
	if (!entry.component.empty())
		return {entry.component, entry.module};
	if (entry.unit == "dfx-mgr-fw-load.service")
		return {"firmware", "pl"};
	if (entry.unit == "msap1-dfx-firmware-rpu-load.service")
		return {"firmware", "rpu"};
	if (entry.unit == "msap1-fpga-acquisition.service")
		return {"fpga-acquisition", entry.module};
	if (entry.unit == "msap1-web-backend.service")
		return {"web-backend", entry.module};
	return {{}, {}};
}

/** Run the bounded, cursor-paginated journal query for @p target. */
DeveloperLogsDto developer_logs(std::string_view target)
{
	const auto params = query_parameters(target);
	mnc::logging::Query query;
	query.limit = log_limit(params);
	query.components = {"fpga-acquisition", "web-backend", "firmware"};
	query.units = {
		"msap1-fpga-acquisition.service",
		"msap1-web-backend.service",
		"dfx-mgr-fw-load.service",
		"msap1-dfx-firmware-rpu-load.service",
	};

	if (const auto item = params.find("component");
	    item != params.end() && !item->second.empty()) {
		if (item->second == "firmware") {
			query.components = {"firmware"};
			query.units = {
				"dfx-mgr-fw-load.service",
				"msap1-dfx-firmware-rpu-load.service",
			};
		} else if (item->second == "fpga-acquisition") {
			query.components = {"fpga-acquisition"};
			query.units = {"msap1-fpga-acquisition.service"};
		} else if (item->second == "web-backend") {
			query.components = {"web-backend"};
			query.units = {"msap1-web-backend.service"};
		} else {
			throw std::invalid_argument("unsupported log component");
		}
	}
	if (const auto item = params.find("module");
	    item != params.end() && !item->second.empty()) {
		/*
		 * Firmware lifecycle entries emitted by PID 1 have a UNIT but no
		 * MNC_MODULE. Translate the two synthetic firmware modules to unit
		 * filters before asking the generic journal reader to match them.
		 */
		if (item->second == "pl" || item->second == "rpu") {
			const auto component = params.find("component");
			if (component != params.end() &&
			    !component->second.empty() &&
			    component->second != "firmware")
				throw std::invalid_argument(
					"PL/RPU modules require the firmware component");
			query.components = {"firmware"};
			query.units = {item->second == "pl"
				? "dfx-mgr-fw-load.service"
				: "msap1-dfx-firmware-rpu-load.service"};
		} else {
			query.module = item->second;
		}
	}
	if (const auto item = params.find("priority");
	    item != params.end() && !item->second.empty()) {
		mnc::logging::Priority priority;
		if (!mnc::logging::parse_priority(item->second, priority))
			throw std::invalid_argument("unsupported log priority");
		query.maximum_priority = priority;
	}
	if (const auto item = params.find("after");
	    item != params.end() && !item->second.empty())
		query.after = mnc::logging::Cursor{item->second};

	mnc::logging::JournalReader reader;
	if (!reader.available())
		throw std::runtime_error("system journal is unavailable");
	const auto entries = reader.read(query);
	DeveloperLogsDto result;
	result.entries.reserve(entries.size());
	if (query.after)
		result.next_cursor = query.after->value;
	for (const auto &entry : entries) {
		auto classified = entry;
		const auto [component, module] = classify_log_entry(entry);
		if (classified.component.empty())
			classified.component = component;
		if (classified.module.empty())
			classified.module = module;
		const auto usec =
			std::chrono::duration_cast<std::chrono::microseconds>(
				classified.timestamp.time_since_epoch())
				.count();
		result.entries.push_back({
			usec,
			classified.cursor.value,
			mnc::logging::priority_name(classified.priority),
			classified.message,
			classified.component,
			classified.module,
			classified.event,
			classified.request_id,
			classified.configuration_generation,
			classified.unit,
			classified.executable,
			classified.source_file,
			classified.source_line,
			classified.source_function,
			mnc::logging::entry_to_json(classified),
		});
		result.next_cursor = classified.cursor.value;
	}
	return result;
}

SocTemperaturesDto soc_temperatures()
{
	const auto readings = msap1::read_soc_temperatures();
	SocTemperaturesDto result{
		std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch())
			.count(),
		{},
	};
	result.sensors.reserve(readings.size());
	for (const auto &reading : readings) {
		result.sensors.push_back({
			reading.zone,
			reading.label,
			reading.available(),
			reading.millidegrees_c.value_or(0),
			reading.celsius(),
		});
	}
	return result;
}

DeveloperAboutDto developer_about()
{
	DeveloperAboutDto result{
		"MD5",
		"Diagnostic file identity only; MD5 is not an integrity or security check",
		{},
	};
	const auto fingerprints = msap1::system_component_fingerprints();
	result.components.reserve(fingerprints.size());
	for (const auto &fingerprint : fingerprints) {
		result.components.push_back({
			fingerprint.id,
			fingerprint.label,
			fingerprint.component_type,
			fingerprint.path,
			fingerprint.available,
			fingerprint.size_bytes,
			fingerprint.md5,
		});
	}
	return result;
}

} // namespace

/**
 * @brief GET /api/v1/developer/temperatures (Admin)
 *
 * Samples the LPD, FPD, and PL SoC temperature sensors (discovered by
 * hwmon label, never by index) and reports them with a sample timestamp.
 *
 * @return 200 with the sensor readings, or 503 when sampling fails.
 */
webengine::Response
get_developer_temperatures(AppContext &, const webengine::RequestContext &)
{
	try {
		return json_response(webengine::http::status::ok,
			soc_temperatures());
	} catch (const std::exception &error) {
		log_api_failure("/api/v1/developer/temperatures", error);
		return error_response(
			webengine::http::status::service_unavailable,
			error.what());
	}
}

/**
 * @brief GET /api/v1/developer/about (Admin)
 *
 * Reports MD5 fingerprints of the installed system components for
 * diagnostic identification.  MD5 is file identity only — it is not an
 * integrity or security check, and the response says so.
 *
 * @return 200 with the fingerprint list.
 */
webengine::Response get_developer_about(AppContext &,
					const webengine::RequestContext &)
{
	return json_response(webengine::http::status::ok, developer_about());
}

/**
 * @brief GET /api/v1/developer/logs (Admin)
 *
 * Bounded, cursor-paginated journald query over the product services.
 * Query parameters: limit (1..500, default 100), component, module,
 * priority, and after (opaque cursor from a previous response).
 *
 * @return 200 with entries and next_cursor, 400 for invalid parameters,
 *         409 when the supplied cursor is no longer valid, or 503 when the
 *         journal is unavailable.
 */
webengine::Response get_developer_logs(AppContext &,
				       const webengine::RequestContext &context)
{
	try {
		const auto target = context.request.target();
		return json_response(webengine::http::status::ok,
			developer_logs(std::string_view{
				target.data(), target.size()}));
	} catch (const std::invalid_argument &error) {
		return error_response(webengine::http::status::bad_request,
			error.what());
	} catch (const std::runtime_error &error) {
		if (std::string_view{error.what()} ==
		    "journal cursor is no longer valid")
			return error_response(webengine::http::status::conflict,
				error.what());
		log_api_failure("/api/v1/developer/logs", error);
		return error_response(
			webengine::http::status::service_unavailable,
			error.what());
	} catch (const std::exception &error) {
		log_api_failure("/api/v1/developer/logs", error);
		return error_response(
			webengine::http::status::service_unavailable,
			error.what());
	}
}

} // namespace msap1::web::api
