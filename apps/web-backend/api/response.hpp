#pragma once

/**
 * @file response.hpp
 * @brief JSON response builders and request-logging helpers shared by every
 *        route translation unit under api/.
 *
 * Keeping these helpers in one header guarantees every endpoint emits the
 * same success body shape, the same {"error": "..."} envelope, and the same
 * structured journald fields.
 */

#include "mnc/logging/logging.hpp"

#include <atomic>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <glaze/glaze.hpp>
#include <webengine/Http.hpp>

namespace msap1::web::api {

/** Journald logger shared by every HTTP route handler. */
inline const mnc::logging::Logger api_log{"web-backend", "http"};

/**
 * @brief Write one structured journal entry through the shared HTTP logger.
 *
 * @param priority Journald priority of the entry.
 * @param message  Human-readable message text.
 * @param event    Stable machine-readable event identifier (MNC_EVENT).
 * @param fields   Additional structured journald fields.
 * @param source   Call site recorded in the journal entry.
 */
inline void log_api_event(
	mnc::logging::Priority priority, std::string_view message,
	std::string_view event,
	std::initializer_list<mnc::logging::Field> fields = {},
	const std::source_location &source = std::source_location::current())
{
	(void)api_log.write(priority, message, event,
			    std::span<const mnc::logging::Field>(
				    fields.begin(), fields.size()),
			    source);
}

/**
 * @brief Allocate a process-unique correlation ID for one request.
 *
 * Attach the ID to every journal entry a mutating handler writes so an
 * operator can associate the request, its side effects, and its outcome.
 */
inline std::string request_id()
{
	static std::atomic<std::uint64_t> sequence{0};
	return std::to_string(++sequence);
}

/**
 * @brief Record a failed API request in the journal.
 *
 * @param route  The route that failed, e.g. "/api/v1/health".
 * @param error  The exception that aborted the handler.
 * @param source Call site recorded in the journal entry.
 */
inline void log_api_failure(
	std::string_view route, const std::exception &error,
	const std::source_location &source = std::source_location::current())
{
	log_api_event(mnc::logging::Priority::warning,
		"API request failed for " + std::string(route) + ": " +
			error.what(),
		"api_request_failed",
		{{"MNC_HTTP_ROUTE", std::string(route)}}, source);
}

/**
 * @brief Serialize @p value with glaze and wrap it in a JSON response.
 *
 * @tparam T     Any glaze-reflectable aggregate (the endpoint's DTO).
 * @param status HTTP status of the response.
 * @param value  Body payload; serialization failure yields a 500 envelope.
 */
template <typename T>
webengine::Response json_response(webengine::http::status status,
				  const T &value)
{
	auto body = glz::write_json(value);
	if (!body)
		return webengine::json(
			webengine::http::status::internal_server_error,
			R"({"error":"JSON serialization failed"})");
	return webengine::json(status, std::move(*body));
}

/**
 * @brief Build the uniform {"error": message} JSON error envelope.
 *
 * @param status  HTTP status of the response.
 * @param message Human-readable failure description.
 */
inline webengine::Response error_response(webengine::http::status status,
					  std::string message)
{
	return json_response(status, glz::obj{"error", std::move(message)});
}

} // namespace msap1::web::api
