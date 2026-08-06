/**
 * @file health_routes.cpp
 * @brief System-level endpoints: session identity, aggregate health, and
 *        product identity.
 */

#include "health_dto.hpp"
#include "response.hpp"
#include "routes.hpp"

#include "msap1/system_identity.hpp"

#include <exception>
#include <string>
#include <utility>

#include <webengine/NginxController.hpp>

namespace msap1::web::api {

namespace {

/** Body of GET /api/v1/session. */
struct SessionDto {
	std::string username;
	std::string role;
};

/** Body of GET /api/v1/health: meter health plus web-stack state. */
struct HealthDto {
	bool healthy;
	AcquisitionHealthDto acquisition;
	AdcHealthDto adc;
	bool frequency_arithmetic_ok;
	bool backend_running;
	bool nginx_running;
};

/** Body of GET /api/v1/about. */
struct SystemAboutDto {
	bool available;
	std::string product;
	std::string operating_system;
	std::string yocto_system_version;
	std::string build_hex;
	std::string software_build_date;
	std::string image_recipe;
	std::string machine;
};

SystemAboutDto system_about()
{
	const auto identity = msap1::read_image_identity();
	auto short_hash = identity.build_hash_short;
	if (short_hash.empty() && identity.build_hash.size() >= 6)
		short_hash = identity.build_hash.substr(0, 6);
	return {
		identity.available,
		"MSAP1",
		"MNCOS",
		identity.distro_version,
		std::move(short_hash),
		identity.build_time,
		identity.image_recipe,
		identity.machine,
	};
}

/** Extend the meter health projection with backend and nginx liveness. */
HealthDto system_health(const msap1::InfoResponse &response,
			const webengine::NginxController &nginx)
{
	const auto meter = meter_health_dto(response);
	const bool nginx_ok = nginx.is_running();
	return {
		meter.healthy && nginx_ok,
		meter.acquisition,
		meter.adc,
		meter.frequency_arithmetic_ok,
		true,
		nginx_ok,
	};
}

} // namespace

/**
 * @brief GET /api/v1/session (Viewer)
 *
 * Identifies the authenticated caller.  The router guarantees a valid
 * session before this handler runs, so context.user is always set.
 *
 * @return 200 with the session's user name and role name.
 */
webengine::Response get_session(AppContext &,
				const webengine::RequestContext &context)
{
	const auto &user = *context.user;
	return json_response(webengine::http::status::ok,
		SessionDto{user.username,
			   webengine::role_name(user.role)});
}

/**
 * @brief GET /api/v1/health (Viewer)
 *
 * Aggregates the metering pipeline health from the acquisition daemon with
 * the web stack's own state (backend liveness and supervised nginx).
 *
 * @return 200 with the aggregate health document, or 503 when the
 *         acquisition daemon is unreachable or reports a failure status.
 */
webengine::Response get_health(AppContext &app,
			       const webengine::RequestContext &)
{
	try {
		const auto response = app.acquisition.information();
		require_acquisition_ok(response.status);
		return json_response(webengine::http::status::ok,
			system_health(response, app.nginx));
	} catch (const std::exception &error) {
		log_api_failure("/api/v1/health", error);
		return error_response(
			webengine::http::status::service_unavailable,
			error.what());
	}
}

/**
 * @brief GET /api/v1/about (Viewer)
 *
 * Reports the product name and installed OS image identity (version,
 * build hash, build date, image recipe, and machine).
 *
 * @return 200 with the identity document; fields are empty and
 *         available=false when the image metadata cannot be read.
 */
webengine::Response get_about(AppContext &,
			      const webengine::RequestContext &)
{
	return json_response(webengine::http::status::ok, system_about());
}

} // namespace msap1::web::api
