/**
 * @file developer_routes.cpp
 * @brief Administrator-only diagnostics: SoC temperatures, component
 *        fingerprints, and the bounded journald query.
 */

#include "openapi.hpp"
#include "response.hpp"
#include "query.hpp"
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
	if (entry.unit == "msap1-mqtt-publisher.service")
		return {"mqtt-publisher", entry.module};
	if (entry.unit == "msap1-modbus-server.service")
		return {"modbus", entry.module};
	return {{}, {}};
}

/** Run the bounded, cursor-paginated journal query for @p target. */
DeveloperLogsDto developer_logs(std::string_view target)
{
	const auto params = query_parameters(target);
	mnc::logging::Query query;
	query.limit = log_limit(params);
	query.components = {
		"fpga-acquisition",
		"web-backend",
		"mqtt-publisher",
		"modbus",
		"firmware",
	};
	query.units = {
		"msap1-fpga-acquisition.service",
		"msap1-web-backend.service",
		"msap1-mqtt-publisher.service",
		"msap1-modbus-server.service",
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
		} else if (item->second == "mqtt-publisher") {
			query.components = {"mqtt-publisher"};
			query.units = {"msap1-mqtt-publisher.service"};
		} else if (item->second == "modbus") {
			query.components = {"modbus"};
			query.units = {"msap1-modbus-server.service"};
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

void document_developer_routes(DocumentedApiRegistry &registry)
{
	using V = webengine::http::verb;
	constexpr auto temperatures = "/api/v1/developer/temperatures";
	registry.add_json_response<SocTemperaturesDto>(V::get, temperatures, 200,
		"SocTemperatures", "Label-discovered SoC temperatures");
	registry.add_error_response(V::get, temperatures, 503,
		"Temperature sampling failed");

	registry.add_json_response<DeveloperAboutDto>(V::get,
		"/api/v1/developer/about", 200, "DeveloperAbout",
		"Diagnostic component fingerprints");

	constexpr auto logs = "/api/v1/developer/logs";
	registry.add_query_parameter(V::get, logs, "component", "string", false,
		"Product component filter",
		{"fpga-acquisition", "web-backend", "mqtt-publisher", "modbus",
		 "firmware"}, "web-backend");
	registry.add_query_parameter(V::get, logs, "module", "string", false,
		"Exact structured-log module filter");
	registry.add_query_parameter(V::get, logs, "priority", "string", false,
		"Maximum syslog severity", {"debug", "info", "notice", "warning",
		 "error", "critical", "alert", "emergency"}, "warning");
	registry.add_query_parameter(V::get, logs, "after", "string", false,
		"Opaque journal continuation cursor");
	registry.add_query_parameter(V::get, logs, "limit", "integer", false,
		"Maximum entries, from 1 through 500", {}, "100");
	registry.add_json_response<DeveloperLogsDto>(V::get, logs, 200,
		"DeveloperLogs", "Bounded journal entries and continuation cursor");
	registry.add_error_response(V::get, logs, 400,
		"A query parameter is invalid");
	registry.add_error_response(V::get, logs, 409,
		"The supplied journal cursor is no longer valid");
	registry.add_error_response(V::get, logs, 503,
		"The system journal is unavailable");
}

} // namespace msap1::web::api
