#include "energy_dto.hpp"
#include "openapi.hpp"
#include "response.hpp"
#include "routes.hpp"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include <glaze/glaze.hpp>

namespace msap1::web::api {
namespace {

struct ResetBodyDto {
	std::string expected_epoch;
	std::string idempotency_key;
};

struct ResetResponseDto {
	std::string reset_epoch;
	bool replayed = false;
	std::string request_id;
};

std::uint64_t parse_epoch(std::string_view value)
{
	std::uint64_t result = 0;
	const auto [end, error] = std::from_chars(value.data(),
		value.data() + value.size(), result);
	if (value.empty() || error != std::errc{} ||
	    end != value.data() + value.size())
		throw std::invalid_argument("expected_epoch must be a uint64 decimal string");
	return result;
}

energy_ledger::ResetRequest reset_request(
	const webengine::RequestContext &context, std::string request_id_value)
{
	ResetBodyDto body;
	if (glz::read_json(body, context.request.body()))
		throw std::invalid_argument("invalid reset request JSON");
	if (body.idempotency_key.empty() || body.idempotency_key.size() > 128)
		throw std::invalid_argument(
			"idempotency_key must contain 1..128 characters");
	if (!context.user)
		throw std::invalid_argument("authenticated reset actor is unavailable");
	return {
		parse_epoch(body.expected_epoch),
		std::move(body.idempotency_key),
		context.user->username,
		std::move(request_id_value),
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::system_clock::now().time_since_epoch()).count(),
	};
}

template<typename Function>
webengine::Response execute_reset(AppContext &app,
	const webengine::RequestContext &context, std::string_view route,
	std::string_view event_prefix, Function function)
{
	const auto correlation = request_id();
	try {
		auto request = reset_request(context, correlation);
		const auto actor = request.actor;
		const auto result = function(app.database, request);
		log_api_event(mnc::logging::Priority::notice,
			std::string(event_prefix) + " reset committed",
			std::string(event_prefix) + "_reset_committed",
			{{"MNC_REQUEST_ID", correlation}, {"MNC_ACTOR", actor},
			 {"MNC_RESET_EPOCH", std::to_string(result.epoch)}});
		return json_response(webengine::http::status::ok,
			ResetResponseDto{std::to_string(result.epoch), result.replayed,
				correlation});
	} catch (const energy_ledger::Unavailable &error) {
		log_api_failure(route, error);
		return error_response(webengine::http::status::service_unavailable,
			error.what());
	} catch (const energy_ledger::Conflict &error) {
		log_api_failure(route, error);
		return error_response(webengine::http::status::conflict, error.what());
	} catch (const std::invalid_argument &error) {
		log_api_failure(route, error);
		return error_response(webengine::http::status::bad_request, error.what());
	} catch (const std::exception &error) {
		log_api_failure(route, error);
		return error_response(webengine::http::status::service_unavailable,
			error.what());
	}
}

} // namespace

webengine::Response get_meter_energy(AppContext &app,
	const webengine::RequestContext &)
{
	try {
		const auto values = app.database.energy();
		if (!values)
			return error_response(webengine::http::status::service_unavailable,
				"no durable ENERGY checkpoint exists");
		return json_response(webengine::http::status::ok, energy_dto(*values));
	} catch (const std::exception &error) {
		log_api_failure("/api/v1/meter/energy", error);
		return error_response(webengine::http::status::service_unavailable,
			error.what());
	}
}

webengine::Response get_meter_demand(AppContext &app,
	const webengine::RequestContext &)
{
	try {
		const auto values = app.database.demand();
		if (!values)
			return error_response(webengine::http::status::service_unavailable,
				"no durable DEMAND checkpoint exists");
		return json_response(webengine::http::status::ok, demand_dto(*values));
	} catch (const std::exception &error) {
		log_api_failure("/api/v1/meter/demand", error);
		return error_response(webengine::http::status::service_unavailable,
			error.what());
	}
}

webengine::Response post_meter_energy_reset(AppContext &app,
	const webengine::RequestContext &context)
{
	return execute_reset(app, context, "/api/v1/meter/energy/reset", "energy",
		[](auto &database, const auto &request) {
			return database.reset_energy(request);
		});
}

webengine::Response post_meter_demand_peaks_reset(AppContext &app,
	const webengine::RequestContext &context)
{
	return execute_reset(app, context, "/api/v1/meter/demand/peaks/reset",
		"demand_peaks", [](auto &database, const auto &request) {
			return database.reset_demand_peaks(request);
		});
}

void document_energy_routes(DocumentedApiRegistry &registry)
{
	using V = webengine::http::verb;
	registry.add_json_response<EnergyResponseDto>(V::get,
		"/api/v1/meter/energy", 200, "MeterEnergy",
		"Authoritative durable energy counters");
	registry.add_error_response(V::get, "/api/v1/meter/energy", 503,
		"No durable energy checkpoint is available");
	registry.add_json_response<DemandResponseDto>(V::get,
		"/api/v1/meter/demand", 200, "MeterDemand",
		"Authoritative demand and peak values");
	registry.add_error_response(V::get, "/api/v1/meter/demand", 503,
		"No durable demand checkpoint is available");

	for (const auto path : {"/api/v1/meter/energy/reset",
		"/api/v1/meter/demand/peaks/reset"}) {
		registry.add_json_request<ResetBodyDto>(V::post, path,
			"MeterResetRequest", "Expected epoch and idempotency key", true,
			R"({"expected_epoch":"0","idempotency_key":"commissioning-1"})");
		registry.add_json_response<ResetResponseDto>(V::post, path, 200,
			"MeterResetResponse", "Committed or replayed reset result");
		registry.add_error_response(V::post, path, 400,
			"The reset request is malformed");
		registry.add_error_response(V::post, path, 409,
			"The expected epoch conflicts with current state");
		registry.add_error_response(V::post, path, 503,
			"The durable ledger is unavailable");
	}
}

} // namespace msap1::web::api
