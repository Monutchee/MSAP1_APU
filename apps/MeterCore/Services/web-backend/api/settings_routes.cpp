/**
 * @file settings_routes.cpp
 * @brief Persistent settings endpoints: read, replace, and factory-reset
 *        the active product settings document.
 */

#include "openapi.hpp"
#include "response.hpp"
#include "routes.hpp"

#include "msap1/settings/settings.hpp"
#include "msap1/settings/settings_ipc.hpp"

#include <exception>
#include <stdexcept>
#include <string>

#include <glaze/glaze.hpp>

namespace msap1::web::api {

namespace {

/** Body of the settings endpoints: document plus authority state. */
struct SettingsDocumentDto {
	std::string content_hash;
	bool recovery_mode = false;
	std::string recovery_reason;
	msap1::settings::ProductSettings settings;
};

/** Body of POST /api/v1/settings/factory-reset. */
struct SettingsFactoryResetDto {
	bool confirmed = false;
};

/** Project a settings-authority reply onto the JSON document DTO. */
SettingsDocumentDto settings_document(
	const msap1::settings::ipc::Response &response)
{
	return {response.content_hash,
		response.status == msap1::settings::ipc::Status::recovery_mode,
		response.status == msap1::settings::ipc::Status::recovery_mode
			? response.message : std::string{},
		msap1::settings::SettingsCodec::decode(response.json)};
}

} // namespace

/**
 * @brief GET /api/v1/settings/active (Viewer)
 *
 * Returns the active persistent settings document with its content hash.
 * In recovery mode the document is still readable and the response carries
 * recovery_mode=true plus the recovery reason.
 *
 * @return 200 with the document, or 503 when the settings authority is
 *         unreachable or rejects the request.
 */
webengine::Response get_active_settings(AppContext &app,
					const webengine::RequestContext &)
{
	try {
		return json_response(webengine::http::status::ok,
			settings_document(app.settings.active()));
	} catch (const std::exception &error) {
		log_api_failure("/api/v1/settings/active", error);
		return error_response(
			webengine::http::status::service_unavailable,
			error.what());
	}
}

/**
 * @brief PUT /api/v1/settings/active (Admin)
 *
 * Replaces the whole active settings document.  The settings authority
 * validates it, hot-applies it to the running services, and persists it
 * atomically; the saved document is returned.
 *
 * @return 200 with the saved document, 400 for invalid JSON or rejected
 *         values, or 409 when the authority refuses the save (for example
 *         while in recovery mode).
 */
webengine::Response put_active_settings(AppContext &app,
					const webengine::RequestContext &context)
{
	try {
		msap1::settings::ProductSettings settings;
		if (glz::read_json(settings, context.request.body()))
			return error_response(
				webengine::http::status::bad_request,
				"invalid settings JSON");
		return json_response(webengine::http::status::ok,
			settings_document(app.settings.save(settings)));
	} catch (const std::invalid_argument &error) {
		return error_response(webengine::http::status::bad_request,
			error.what());
	} catch (const std::exception &error) {
		log_api_failure("/api/v1/settings/active", error);
		return error_response(webengine::http::status::conflict,
			error.what());
	}
}

/**
 * @brief POST /api/v1/settings/factory-reset (Admin)
 *
 * Restores the packaged factory settings document.  The body must contain
 * {"confirmed": true}; this is the only way out of settings recovery mode.
 *
 * @return 200 with the restored document, 400 when confirmation is
 *         missing, or 409 when the authority rejects the reset.
 */
webengine::Response post_factory_reset(AppContext &app,
				       const webengine::RequestContext &context)
{
	try {
		SettingsFactoryResetDto reset;
		if (glz::read_json(reset, context.request.body()) ||
		    !reset.confirmed)
			return error_response(
				webengine::http::status::bad_request,
				"factory reset confirmation is required");
		const auto response = app.settings.factory_reset(true);
		return json_response(webengine::http::status::ok,
			settings_document(response));
	} catch (const std::exception &error) {
		log_api_failure("/api/v1/settings/factory-reset", error);
		return error_response(webengine::http::status::conflict,
			error.what());
	}
}

void document_settings_routes(DocumentedApiRegistry &registry)
{
	using V = webengine::http::verb;
	constexpr auto active = "/api/v1/settings/active";
	registry.add_json_response<SettingsDocumentDto>(V::get, active, 200,
		"SettingsDocument", "Active settings and authority state");
	registry.add_error_response(V::get, active, 503,
		"The settings authority is unavailable");
	registry.add_json_request<msap1::settings::ProductSettings>(V::put, active,
		"ProductSettings", "Complete replacement settings document");
	registry.add_json_response<SettingsDocumentDto>(V::put, active, 200,
		"SettingsDocument", "Validated, applied, and saved settings");
	registry.add_error_response(V::put, active, 400,
		"The settings JSON or values are invalid");
	registry.add_error_response(V::put, active, 409,
		"The settings authority rejected the save");

	constexpr auto reset = "/api/v1/settings/factory-reset";
	registry.add_json_request<SettingsFactoryResetDto>(V::post, reset,
		"FactoryResetRequest", "Explicit factory-reset confirmation", true,
		R"({"confirmed":true})");
	registry.add_json_response<SettingsDocumentDto>(V::post, reset, 200,
		"SettingsDocument", "Restored factory settings document");
	registry.add_error_response(V::post, reset, 400,
		"Explicit confirmation was not supplied");
	registry.add_error_response(V::post, reset, 409,
		"The settings authority rejected the reset");
}

} // namespace msap1::web::api
