#pragma once

/**
 * @file routes.hpp
 * @brief The single authoritative table of every external JSON API route.
 *
 * Each RouteEntry binds one HTTP method and path to the minimum session
 * role and the handler function that implements it.  Handlers are plain
 * functions grouped by purpose into the route translation units under
 * api/; the table itself is constexpr, so the complete API surface is
 * visible — and reviewable — in one place at compile time.
 *
 * To add an endpoint:
 *   1. implement a handler with the RouteHandler signature in the matching
 *      *_routes.cpp (or a new module),
 *   2. declare it below next to its module,
 *   3. append one RouteEntry to route_table.
 *
 * register_routes() wires the whole table into the WebEngine at startup.
 */

#include "app_context.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <webengine/Http.hpp>
#include <webengine/Role.hpp>
#include <webengine/WebEngine.hpp>

namespace msap1::web::api {

/**
 * Signature every route handler implements.
 *
 * A handler is a pure request → response function: it receives the shared
 * service dependencies and the authenticated request context, and returns
 * the complete HTTP response.  Handlers never touch global state, which is
 * what allows them to sit behind constexpr function pointers.
 */
using RouteHandler =
	webengine::Response (*)(AppContext &,
				const webengine::RequestContext &);

/** One row of the API routing table. */
struct RouteEntry {
	webengine::http::verb method; /**< HTTP method, matched exactly. */
	std::string_view path;	      /**< Exact request path. */
	webengine::Role min_role;     /**< Minimum session role required. */
	RouteHandler handler;	      /**< Implementation, one route module. */
	std::string_view summary;     /**< One-line human description. */
};

/* ── health_routes.cpp — session, aggregate health, product identity ───── */

/** @brief GET /api/v1/session — authenticated user name and role. */
webengine::Response get_session(AppContext &,
				const webengine::RequestContext &);
/** @brief GET /api/v1/health — meter health plus backend/nginx state. */
webengine::Response get_health(AppContext &,
			       const webengine::RequestContext &);
/** @brief GET /api/v1/about — product and OS image identity. */
webengine::Response get_about(AppContext &,
			      const webengine::RequestContext &);

/* ── meter_routes.cpp — metering health, readings, frequency config ────── */

/** @brief GET /api/v1/meter/health — metering pipeline health. */
webengine::Response get_meter_health(AppContext &,
				     const webengine::RequestContext &);
/** @brief GET /api/v1/meter/readings — latest RMS/frequency record. */
webengine::Response get_meter_readings(AppContext &,
				       const webengine::RequestContext &);
/** @brief GET /api/v1/meter/power-quality — Urms(1/2) record and event. */
webengine::Response get_meter_power_quality(AppContext &,
					    const webengine::RequestContext &);
/** @brief GET /api/v1/meter/harmonics — latest complete M16 spectrum. */
webengine::Response get_meter_harmonics(AppContext &,
					const webengine::RequestContext &);

/** @brief GET /api/v1/meter/single-cycle — SCYC diagnostic snapshot. */
webengine::Response get_meter_single_cycle(AppContext &,
					   const webengine::RequestContext &);

/** @brief GET /api/v1/meter/aggregate — newest 150/180-cycle aggregate. */
webengine::Response get_meter_aggregate(AppContext &,
					const webengine::RequestContext &);
/** @brief GET /api/v1/meter/minutes-10 — newest aligned ten-minute block. */
webengine::Response get_meter_ten_minute(AppContext &,
					 const webengine::RequestContext &);
/** @brief GET /api/v1/meter/hours-2 — newest finalized two-hour block. */
webengine::Response get_meter_two_hour(AppContext &,
				       const webengine::RequestContext &);
/** @brief GET /api/v1/meter/minutes-10/live — open non-normative preview. */
webengine::Response get_meter_ten_minute_live(AppContext &,
					      const webengine::RequestContext &);
/** @brief GET /api/v1/meter/hours-2/live — open non-normative preview. */
webengine::Response get_meter_two_hour_live(AppContext &,
					    const webengine::RequestContext &);
/** @brief GET /api/v1/meter/configuration/frequency — active config. */
webengine::Response
get_frequency_configuration(AppContext &,
			    const webengine::RequestContext &);
/** @brief PUT /api/v1/meter/configuration/frequency — persist and apply. */
webengine::Response
put_frequency_configuration(AppContext &,
			    const webengine::RequestContext &);

/* ── adc_routes.cpp — ADC source, simulator, capture control ───────────── */

/** @brief GET /api/v1/adc/source — active ADC input source. */
webengine::Response get_adc_source(AppContext &,
				   const webengine::RequestContext &);
/** @brief PUT /api/v1/adc/source — switch physical/simulator source. */
webengine::Response put_adc_source(AppContext &,
				   const webengine::RequestContext &);
/** @brief GET /api/v1/adc/simulator — simulator config and health. */
webengine::Response get_adc_simulator(AppContext &,
				      const webengine::RequestContext &);
/** @brief PUT /api/v1/adc/simulator — persist and apply simulator config. */
webengine::Response put_adc_simulator(AppContext &,
				      const webengine::RequestContext &);
/** @brief POST /api/v1/adc/simulator/event — arm/cancel an amplitude event. */
webengine::Response post_adc_simulator_event(AppContext &,
					     const webengine::RequestContext &);

/** @brief GET /api/v1/adc/simulator/event — event sequencer state. */
webengine::Response get_adc_simulator_event(AppContext &,
					    const webengine::RequestContext &);

/** @brief GET /api/v1/adc/capture — report whether capture is running. */
webengine::Response get_adc_capture(AppContext &,
				    const webengine::RequestContext &);
/** @brief PUT /api/v1/adc/capture — start the capture pipeline. */
webengine::Response put_adc_capture(AppContext &,
				    const webengine::RequestContext &);
/** @brief DELETE /api/v1/adc/capture — stop the capture pipeline. */
webengine::Response delete_adc_capture(AppContext &,
				       const webengine::RequestContext &);

/* ── waveform_routes.cpp — waveform history and manual triggers ────────── */

/** @brief GET /api/v1/waveforms — waveform engine status and sessions. */
webengine::Response get_waveforms(AppContext &,
				  const webengine::RequestContext &);
/** @brief POST /api/v1/waveforms/trigger — start a manual capture. */
webengine::Response post_waveform_trigger(AppContext &,
					  const webengine::RequestContext &);
/** @brief DELETE /api/v1/waveforms — delete one completed session. */
webengine::Response
delete_waveform_session(AppContext &, const webengine::RequestContext &);

/* ── settings_routes.cpp — persistent settings authority ───────────────── */

/** @brief GET /api/v1/settings/active — active settings document. */
webengine::Response get_active_settings(AppContext &,
					const webengine::RequestContext &);
/** @brief PUT /api/v1/settings/active — validate, apply, and persist. */
webengine::Response put_active_settings(AppContext &,
					const webengine::RequestContext &);
/** @brief POST /api/v1/settings/factory-reset — restore factory defaults. */
webengine::Response post_factory_reset(AppContext &,
				       const webengine::RequestContext &);

/* ── developer_routes.cpp — administrator diagnostics ──────────────────── */

/** @brief GET /api/v1/developer/temperatures — SoC temperature sensors. */
webengine::Response
get_developer_temperatures(AppContext &, const webengine::RequestContext &);
/** @brief GET /api/v1/developer/about — component fingerprints. */
webengine::Response get_developer_about(AppContext &,
					const webengine::RequestContext &);
/** @brief GET /api/v1/developer/logs — bounded journald query. */
webengine::Response get_developer_logs(AppContext &,
				       const webengine::RequestContext &);

/* ── database_routes.cpp — administrator storage policy and status ────── */
webengine::Response get_developer_database(AppContext &,
					   const webengine::RequestContext &);
webengine::Response put_developer_database(AppContext &,
					   const webengine::RequestContext &);
webengine::Response post_developer_database_maintenance(AppContext &,
						const webengine::RequestContext &);

/* ── history_routes.cpp — authenticated historical meter queries ─────── */
webengine::Response get_history_capabilities(AppContext &,
					      const webengine::RequestContext &);
webengine::Response post_history_query(AppContext &,
				       const webengine::RequestContext &);
webengine::Response get_history_health(AppContext &,
				       const webengine::RequestContext &);

/* ── mqtt_routes.cpp — MQTT publisher configuration and credentials ───── */
webengine::Response get_mqtt_capabilities(AppContext &,
	const webengine::RequestContext &);
webengine::Response get_mqtt_configuration(AppContext &,
	const webengine::RequestContext &);
webengine::Response put_mqtt_configuration(AppContext &,
	const webengine::RequestContext &);
webengine::Response get_mqtt_status(AppContext &,
	const webengine::RequestContext &);
webengine::Response put_mqtt_password(AppContext &,
	const webengine::RequestContext &);
webengine::Response delete_mqtt_password(AppContext &,
	const webengine::RequestContext &);
webengine::Response put_mqtt_private_key_passphrase(AppContext &,
	const webengine::RequestContext &);
webengine::Response delete_mqtt_private_key_passphrase(AppContext &,
	const webengine::RequestContext &);
webengine::Response delete_mqtt_ca(AppContext &,
	const webengine::RequestContext &);
webengine::Response delete_mqtt_client_certificate(AppContext &,
	const webengine::RequestContext &);
webengine::Response delete_mqtt_client_key(AppContext &,
	const webengine::RequestContext &);
webengine::Response upload_mqtt_ca(AppContext &,
	const webengine::RequestContext &, const webengine::FileUpload &);
webengine::Response upload_mqtt_client_certificate(AppContext &,
	const webengine::RequestContext &, const webengine::FileUpload &);
webengine::Response upload_mqtt_client_key(AppContext &,
	const webengine::RequestContext &, const webengine::FileUpload &);
std::optional<webengine::FileDownload> download_mqtt_ca(AppContext &,
	const webengine::RequestContext &);
std::optional<webengine::FileDownload> download_mqtt_client_certificate(
	AppContext &, const webengine::RequestContext &);

/**
 * @brief Every route of the external JSON API, grouped by module.
 *
 * The table is the one place a reviewer needs to look to audit the API
 * surface and its role policy.  Order is presentation only; the router
 * matches (method, path) exactly.
 */
inline constexpr auto route_table = std::to_array<RouteEntry>({
	/* System (health_routes.cpp) */
	{webengine::http::verb::get, "/api/v1/session",
	 webengine::Role::Viewer, &get_session,
	 "Authenticated session user and role"},
	{webengine::http::verb::get, "/api/v1/health",
	 webengine::Role::Viewer, &get_health,
	 "Aggregate meter, backend, and nginx health"},
	{webengine::http::verb::get, "/api/v1/about",
	 webengine::Role::Viewer, &get_about,
	 "Product and OS image identity"},

	/* Metering (meter_routes.cpp) */
	{webengine::http::verb::get, "/api/v1/meter/health",
	 webengine::Role::Viewer, &get_meter_health,
	 "Metering pipeline health"},
	{webengine::http::verb::get, "/api/v1/meter/readings",
	 webengine::Role::Viewer, &get_meter_readings,
	 "Latest RMS and frequency readings"},
	{webengine::http::verb::get, "/api/v1/meter/aggregate",
	 webengine::Role::Viewer, &get_meter_aggregate,
	 "Newest 150/180-cycle aggregate meter values"},
	{webengine::http::verb::get, "/api/v1/meter/minutes-10",
	 webengine::Role::Viewer, &get_meter_ten_minute,
	 "Newest clock-aligned ten-minute aggregate"},
	{webengine::http::verb::get, "/api/v1/meter/hours-2",
	 webengine::Role::Viewer, &get_meter_two_hour,
	 "Newest finalized two-hour aggregate"},
	{webengine::http::verb::get, "/api/v1/meter/minutes-10/live",
	 webengine::Role::Viewer, &get_meter_ten_minute_live,
	 "Newest open non-normative ten-minute preview"},
	{webengine::http::verb::get, "/api/v1/meter/hours-2/live",
	 webengine::Role::Viewer, &get_meter_two_hour_live,
	 "Newest open non-normative two-hour preview"},
	{webengine::http::verb::get, "/api/v1/meter/single-cycle",
	 webengine::Role::Viewer, &get_meter_single_cycle,
	 "Latest single-cycle diagnostic (RMS, VLL, per-phase power)"},
	{webengine::http::verb::get, "/api/v1/meter/power-quality",
	 webengine::Role::Viewer, &get_meter_power_quality,
	 "Latest Urms(1/2) record and the newest sag/swell/interruption"},
	{webengine::http::verb::get, "/api/v1/meter/harmonics",
	 webengine::Role::Viewer, &get_meter_harmonics,
	 "Latest complete seven-channel harmonic subgroup spectrum"},
	{webengine::http::verb::get, "/api/v1/meter/configuration/frequency",
	 webengine::Role::Viewer, &get_frequency_configuration,
	 "Active frequency measurement configuration"},
	{webengine::http::verb::put, "/api/v1/meter/configuration/frequency",
	 webengine::Role::Admin, &put_frequency_configuration,
	 "Persist and hot-apply frequency configuration"},

	/* ADC control (adc_routes.cpp) */
	{webengine::http::verb::get, "/api/v1/adc/source",
	 webengine::Role::Viewer, &get_adc_source,
	 "Active ADC input source"},
	{webengine::http::verb::put, "/api/v1/adc/source",
	 webengine::Role::Admin, &put_adc_source,
	 "Switch between physical ADC and simulator"},
	{webengine::http::verb::get, "/api/v1/adc/simulator",
	 webengine::Role::Viewer, &get_adc_simulator,
	 "ADC simulator configuration and health"},
	{webengine::http::verb::put, "/api/v1/adc/simulator",
	 webengine::Role::Admin, &put_adc_simulator,
	 "Persist and hot-apply simulator configuration"},
	{webengine::http::verb::get, "/api/v1/adc/simulator/event",
	 webengine::Role::Viewer, &get_adc_simulator_event,
	 "Simulator amplitude-event sequencer state"},
	{webengine::http::verb::post, "/api/v1/adc/simulator/event",
	 webengine::Role::Admin, &post_adc_simulator_event,
	 "Arm, cancel, or clear a simulator sag/swell/interruption"},
	{webengine::http::verb::get, "/api/v1/adc/capture",
	 webengine::Role::Viewer, &get_adc_capture,
	 "Report whether ADC capture is running"},
	{webengine::http::verb::put, "/api/v1/adc/capture",
	 webengine::Role::Admin, &put_adc_capture,
	 "Start the ADC capture pipeline"},
	{webengine::http::verb::delete_, "/api/v1/adc/capture",
	 webengine::Role::Admin, &delete_adc_capture,
	 "Stop the ADC capture pipeline"},

	/* Waveforms (waveform_routes.cpp) */
	{webengine::http::verb::get, "/api/v1/waveforms",
	 webengine::Role::Viewer, &get_waveforms,
	 "Waveform engine status and capture sessions"},
	{webengine::http::verb::post, "/api/v1/waveforms/trigger",
	 webengine::Role::Admin, &post_waveform_trigger,
	 "Trigger a manual waveform capture"},
	{webengine::http::verb::delete_, "/api/v1/waveforms",
	 webengine::Role::Admin, &delete_waveform_session,
	 "Delete one completed waveform session"},

	/* Settings (settings_routes.cpp) */
	{webengine::http::verb::get, "/api/v1/settings/active",
	 webengine::Role::Viewer, &get_active_settings,
	 "Active persistent settings document"},
	{webengine::http::verb::put, "/api/v1/settings/active",
	 webengine::Role::Admin, &put_active_settings,
	 "Validate, hot-apply, and persist settings"},
	{webengine::http::verb::post, "/api/v1/settings/factory-reset",
	 webengine::Role::Admin, &post_factory_reset,
	 "Restore the packaged factory settings"},

	/* Developer diagnostics (developer_routes.cpp) */
	{webengine::http::verb::get, "/api/v1/developer/temperatures",
	 webengine::Role::Admin, &get_developer_temperatures,
	 "SoC temperature sensor readings"},
	{webengine::http::verb::get, "/api/v1/developer/about",
	 webengine::Role::Admin, &get_developer_about,
	 "Diagnostic component fingerprints"},
	{webengine::http::verb::get, "/api/v1/developer/logs",
	 webengine::Role::Admin, &get_developer_logs,
	 "Bounded, cursor-paginated journal query"},
	{webengine::http::verb::get, "/api/v1/developer/database",
	 webengine::Role::Admin, &get_developer_database,
	 "Database policies and live stream/historian status"},
	{webengine::http::verb::put, "/api/v1/developer/database",
	 webengine::Role::Admin, &put_developer_database,
	 "Persist and hot-apply database storage policies"},
	{webengine::http::verb::post,
	 "/api/v1/developer/database/maintenance",
	 webengine::Role::Admin, &post_developer_database_maintenance,
	 "Clear historian datasets or recreate historian storage"},

	/* Historical meter data (history_routes.cpp) */
	{webengine::http::verb::get, "/api/v1/meter/history/capabilities",
	 webengine::Role::Viewer, &get_history_capabilities,
	 "Historian periods, attributes, and query bounds"},
	{webengine::http::verb::post, "/api/v1/meter/history/query",
	 webengine::Role::Viewer, &post_history_query,
	 "Bounded historical meter-data query"},
	{webengine::http::verb::get, "/api/v1/meter/history/health",
	 webengine::Role::Viewer, &get_history_health,
	 "Historian service health"},

	/* MQTT publishing (mqtt_routes.cpp) */
	{webengine::http::verb::get, "/api/v1/mqtt/capabilities",
	 webengine::Role::Admin, &get_mqtt_capabilities,
	 "MQTT-selectable meter periods and attributes"},
	{webengine::http::verb::get, "/api/v1/mqtt/configuration",
	 webengine::Role::Admin, &get_mqtt_configuration,
	 "Active MQTT settings and credential presence"},
	{webengine::http::verb::put, "/api/v1/mqtt/configuration",
	 webengine::Role::Admin, &put_mqtt_configuration,
	 "Persist MQTT publication settings"},
	{webengine::http::verb::get, "/api/v1/mqtt/status",
	 webengine::Role::Admin, &get_mqtt_status,
	 "MQTT connection and per-publication status"},
	{webengine::http::verb::put,
	 "/api/v1/mqtt/credentials/password", webengine::Role::Admin,
	 &put_mqtt_password, "Replace the MQTT broker password"},
	{webengine::http::verb::delete_,
	 "/api/v1/mqtt/credentials/password", webengine::Role::Admin,
	 &delete_mqtt_password, "Remove the MQTT broker password"},
	{webengine::http::verb::put,
	 "/api/v1/mqtt/credentials/private-key-passphrase",
	 webengine::Role::Admin, &put_mqtt_private_key_passphrase,
	 "Replace the MQTT private-key passphrase"},
	{webengine::http::verb::delete_,
	 "/api/v1/mqtt/credentials/private-key-passphrase",
	 webengine::Role::Admin, &delete_mqtt_private_key_passphrase,
	 "Remove the MQTT private-key passphrase"},
	{webengine::http::verb::delete_, "/api/v1/mqtt/tls/ca",
	 webengine::Role::Admin, &delete_mqtt_ca, "Remove the MQTT CA asset"},
	{webengine::http::verb::delete_,
	 "/api/v1/mqtt/tls/client-certificate", webengine::Role::Admin,
	 &delete_mqtt_client_certificate,
	 "Remove the MQTT client certificate"},
	{webengine::http::verb::delete_, "/api/v1/mqtt/tls/client-key",
	 webengine::Role::Admin, &delete_mqtt_client_key,
	 "Remove the upload-only MQTT private key"},
});

/**
 * @brief Register every route_table entry with the engine.
 *
 * Each registration adapts the table's plain function pointer to the
 * engine's std::function handler by capturing the shared AppContext.
 *
 * @param engine  The WebEngine accepting API registrations.
 * @param context Handler dependencies; must outlive the engine.
 */
inline void register_routes(webengine::WebEngine &engine, AppContext &context)
{
	for (const auto &route : route_table)
		engine.add_api(route.method, std::string(route.path),
			[&context, handler = route.handler](
				const webengine::RequestContext &request) {
				return handler(context, request);
			},
			route.min_role);

	constexpr std::size_t certificate_limit = 1024 * 1024;
	const std::vector<std::string> certificate_types{
		"application/octet-stream", "application/x-pem-file",
		"application/pkix-cert", "application/x-x509-ca-cert"};
	engine.add_file_upload("/api/v1/mqtt/tls/ca",
		[&context](const auto &request, const auto &file) {
			return upload_mqtt_ca(context, request, file);
		}, webengine::Role::Admin, certificate_limit, certificate_types);
	engine.add_file_download("/api/v1/mqtt/tls/ca",
		[&context](const auto &request) {
			return download_mqtt_ca(context, request);
		}, webengine::Role::Admin);
	engine.add_file_upload("/api/v1/mqtt/tls/client-certificate",
		[&context](const auto &request, const auto &file) {
			return upload_mqtt_client_certificate(context, request, file);
		}, webengine::Role::Admin, certificate_limit, certificate_types);
	engine.add_file_download("/api/v1/mqtt/tls/client-certificate",
		[&context](const auto &request) {
			return download_mqtt_client_certificate(context, request);
		}, webengine::Role::Admin);
	engine.add_file_upload("/api/v1/mqtt/tls/client-key",
		[&context](const auto &request, const auto &file) {
			return upload_mqtt_client_key(context, request, file);
		}, webengine::Role::Admin, certificate_limit, certificate_types);
}

} // namespace msap1::web::api
