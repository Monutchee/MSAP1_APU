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
 *   3. append one RouteEntry to route_table, and
 *   4. attach its typed schemas, parameters, statuses, content types, and
 *      examples in that module's document_*_routes() decorator.
 *
 * register_routes() wires the whole table into WebEngine at startup, while
 * the build-only OpenAPI adapter imports that same table and rejects missing
 * documentation metadata.
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

/* ── attribute_routes.cpp — canonical meter attribute capabilities ────── */

/** @brief GET /api/v1/meter/attributes?usage=snapshot|historian. */
webengine::Response get_meter_attributes(AppContext &,
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
/** @brief GET /api/v1/meter/power-quality/events — durable M18 catalogue. */
webengine::Response get_power_quality_events(AppContext &,
	const webengine::RequestContext &);
/** @brief DELETE /api/v1/meter/power-quality/events — remove catalogue rows. */
webengine::Response delete_power_quality_events(AppContext &,
	const webengine::RequestContext &);
/** @brief GET /api/v1/meter/flicker — latest live/Pst/Plt records. */
webengine::Response get_meter_flicker(AppContext &,
	const webengine::RequestContext &);
/** @brief GET /api/v1/meter/mains-signalling — latest carrier observation. */
webengine::Response get_meter_mains_signalling(AppContext &,
	const webengine::RequestContext &);
/** @brief GET /api/v1/meter/harmonics — latest complete M16 spectrum. */
webengine::Response get_meter_harmonics(AppContext &,
					const webengine::RequestContext &);
webengine::Response get_meter_energy(AppContext &,
	const webengine::RequestContext &);
webengine::Response get_meter_demand(AppContext &,
	const webengine::RequestContext &);
webengine::Response post_meter_energy_reset(AppContext &,
	const webengine::RequestContext &);
webengine::Response post_meter_demand_peaks_reset(AppContext &,
	const webengine::RequestContext &);

/** @brief GET /api/v1/meter/single-cycle — SCYC diagnostic snapshot. */
webengine::Response get_meter_single_cycle(AppContext &,
					   const webengine::RequestContext &);

/** @brief GET /api/v1/meter/aggregate — newest 150/180-cycle aggregate. */
webengine::Response get_meter_aggregate(AppContext &,
					const webengine::RequestContext &);
/** @brief GET /api/v1/meter/frequency-10s — standardized UTC result. */
webengine::Response get_meter_frequency_10s(AppContext &,
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
/** @brief GET /api/v1/waveforms/session — exact capture UUID lookup. */
webengine::Response get_waveform_session(AppContext &,
	const webengine::RequestContext &);
/** @brief POST /api/v1/waveforms/sessions/lookup — bounded UUID lookup. */
webengine::Response post_waveform_session_lookup(AppContext &,
	const webengine::RequestContext &);
/** @brief POST /api/v1/waveforms/trigger — start a manual capture. */
webengine::Response post_waveform_trigger(AppContext &,
					  const webengine::RequestContext &);
/** @brief DELETE /api/v1/waveforms — delete one completed session. */
webengine::Response
delete_waveform_session(AppContext &, const webengine::RequestContext &);
/** @brief GET /api/v1/waveforms/export — streamed event-specific MNCWF. */
webengine::HandlerResult export_waveform_event(
	AppContext &, const webengine::RequestContext &);

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

/* ── data_logging_routes.cpp — M19 jobs, channels, and generated files ── */
webengine::Response get_data_logging_configuration(AppContext &,
	const webengine::RequestContext &);
webengine::Response put_data_logging_configuration(AppContext &,
	const webengine::RequestContext &);
webengine::Response get_data_logging_status(AppContext &,
	const webengine::RequestContext &);
webengine::Response get_data_logging_artifacts(AppContext &,
	const webengine::RequestContext &);
webengine::Response get_data_logging_artifact(AppContext &,
	const webengine::RequestContext &);
webengine::Response get_data_logging_preview(AppContext &,
	const webengine::RequestContext &);
webengine::Response post_data_logging_retry(AppContext &,
	const webengine::RequestContext &);
webengine::Response delete_data_logging_artifacts(AppContext &,
	const webengine::RequestContext &);
webengine::Response post_data_logging_channel_test(AppContext &,
	const webengine::RequestContext &);
webengine::Response get_data_logging_materials(AppContext &,
	const webengine::RequestContext &);
webengine::Response put_data_logging_credential(AppContext &,
	const webengine::RequestContext &);
webengine::Response delete_data_logging_credential(AppContext &,
	const webengine::RequestContext &);
webengine::Response delete_data_logging_asset(AppContext &,
	const webengine::RequestContext &);
webengine::Response upload_data_logging_asset(AppContext &,
	const webengine::RequestContext &, const webengine::FileUpload &);
webengine::HandlerResult download_data_logging_artifact(AppContext &,
	const webengine::RequestContext &);

/* ── documentation_routes.cpp — immutable build documentation ───────── */
std::optional<webengine::FileDownload> download_openapi_document(AppContext &,
	const webengine::RequestContext &);
std::optional<webengine::FileDownload> download_modbus_document(AppContext &,
	const webengine::RequestContext &);

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
	{webengine::http::verb::get, "/api/v1/meter/attributes",
	 webengine::Role::Viewer, &get_meter_attributes,
	 "Canonical period-aware meter attribute capabilities"},
	{webengine::http::verb::get, "/api/v1/meter/readings",
	 webengine::Role::Viewer, &get_meter_readings,
	 "Latest RMS and frequency readings"},
	{webengine::http::verb::get, "/api/v1/meter/aggregate",
	 webengine::Role::Viewer, &get_meter_aggregate,
	 "Newest 150/180-cycle aggregate meter values"},
	{webengine::http::verb::get, "/api/v1/meter/frequency-10s",
	 webengine::Role::Viewer, &get_meter_frequency_10s,
	 "Newest UTC-aligned IEC ten-second frequency result"},
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
	{webengine::http::verb::get, "/api/v1/meter/power-quality/events",
	 webengine::Role::Viewer, &get_power_quality_events,
	 "Durable M18 power-quality event catalogue and detail"},
	{webengine::http::verb::delete_,
	 "/api/v1/meter/power-quality/events",
	 webengine::Role::Admin, &delete_power_quality_events,
	 "Delete selected or all durable power-quality catalogue events"},
	{webengine::http::verb::get, "/api/v1/meter/flicker",
	 webengine::Role::Viewer, &get_meter_flicker,
	 "Latest independent flicker live, Pst, and Plt values"},
	{webengine::http::verb::get, "/api/v1/meter/mains-signalling",
	 webengine::Role::Viewer, &get_meter_mains_signalling,
	 "Latest mains-signalling carrier observation"},
	{webengine::http::verb::get, "/api/v1/meter/harmonics",
	 webengine::Role::Viewer, &get_meter_harmonics,
	 "Latest complete seven-channel harmonic subgroup spectrum"},
	{webengine::http::verb::get, "/api/v1/meter/energy",
	 webengine::Role::Viewer, &get_meter_energy,
	 "Durable four-quadrant lifetime energy"},
	{webengine::http::verb::get, "/api/v1/meter/demand",
	 webengine::Role::Viewer, &get_meter_demand,
	 "Latest durable configured active demand and peaks"},
	{webengine::http::verb::post, "/api/v1/meter/energy/reset",
	 webengine::Role::Admin, &post_meter_energy_reset,
	 "Reset all authoritative energy counters"},
	{webengine::http::verb::post, "/api/v1/meter/demand/peaks/reset",
	 webengine::Role::Admin, &post_meter_demand_peaks_reset,
	 "Reset all authoritative demand peaks"},
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
	 "Waveform engine status and paged capture sessions"},
	{webengine::http::verb::get, "/api/v1/waveforms/session",
	 webengine::Role::Viewer, &get_waveform_session,
	 "Resolve one capture UUID across the waveform archive"},
	{webengine::http::verb::post, "/api/v1/waveforms/sessions/lookup",
	 webengine::Role::Viewer, &post_waveform_session_lookup,
	 "Resolve up to 32 capture UUIDs across the waveform archive"},
	{webengine::http::verb::post, "/api/v1/waveforms/trigger",
	 webengine::Role::Admin, &post_waveform_trigger,
	 "Trigger a manual waveform capture"},
	{webengine::http::verb::delete_, "/api/v1/waveforms",
	 webengine::Role::Admin, &delete_waveform_session,
	 "Delete one completed waveform session or all inactive sessions"},

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

	/* M19 Data Logging (data_logging_routes.cpp) */
	{webengine::http::verb::get, "/api/v1/data-logging/configuration",
	 webengine::Role::Admin, &get_data_logging_configuration,
	 "Data Logging jobs, channels, storage policy, and material presence"},
	{webengine::http::verb::put, "/api/v1/data-logging/configuration",
	 webengine::Role::Admin, &put_data_logging_configuration,
	 "Validate and persist Data Logging jobs and Data Channels"},
	{webengine::http::verb::get, "/api/v1/data-logging/status",
	 webengine::Role::Viewer, &get_data_logging_status,
	 "Data Sender jobs, channels, queue, archive, and storage status"},
	{webengine::http::verb::get, "/api/v1/data-logging/artifacts",
	 webengine::Role::Viewer, &get_data_logging_artifacts,
	 "Bounded generated-file listing"},
	{webengine::http::verb::delete_, "/api/v1/data-logging/artifacts",
	 webengine::Role::Admin, &delete_data_logging_artifacts,
	 "Delete selected or all generated files with unsent confirmation"},
	{webengine::http::verb::get, "/api/v1/data-logging/artifact",
	 webengine::Role::Viewer, &get_data_logging_artifact,
	 "Generated-file manifest and per-channel delivery detail"},
	{webengine::http::verb::get, "/api/v1/data-logging/artifacts/preview",
	 webengine::Role::Viewer, &get_data_logging_preview,
	 "Bounded generated-file text preview"},
	{webengine::http::verb::post, "/api/v1/data-logging/artifacts/retry",
	 webengine::Role::Admin, &post_data_logging_retry,
	 "Retry selected generated-file deliveries"},
	{webengine::http::verb::post, "/api/v1/data-logging/channels/test",
	 webengine::Role::Admin, &post_data_logging_channel_test,
	 "Send a clearly marked zero-data probe through a saved channel"},
	{webengine::http::verb::get, "/api/v1/data-logging/channel-materials",
	 webengine::Role::Admin, &get_data_logging_materials,
	 "Channel credential and verification-asset presence"},
	{webengine::http::verb::put, "/api/v1/data-logging/channel-credential",
	 webengine::Role::Admin, &put_data_logging_credential,
	 "Replace one channel-scoped secret"},
	{webengine::http::verb::delete_,
	 "/api/v1/data-logging/channel-credential", webengine::Role::Admin,
	 &delete_data_logging_credential, "Remove one channel-scoped secret"},
	{webengine::http::verb::delete_, "/api/v1/data-logging/channel-asset",
	 webengine::Role::Admin, &delete_data_logging_asset,
	 "Remove one channel certificate, key, or known-host asset"},
});

struct RequiredRouteContract {
	webengine::http::verb method;
	std::string_view path;
	webengine::Role role;
};

inline constexpr auto m19_route_contract =
	std::to_array<RequiredRouteContract>({
		{webengine::http::verb::get, "/api/v1/meter/attributes",
		 webengine::Role::Viewer},
		{webengine::http::verb::get,
		 "/api/v1/data-logging/configuration", webengine::Role::Admin},
		{webengine::http::verb::put,
		 "/api/v1/data-logging/configuration", webengine::Role::Admin},
		{webengine::http::verb::get, "/api/v1/data-logging/status",
		 webengine::Role::Viewer},
		{webengine::http::verb::get, "/api/v1/data-logging/artifacts",
		 webengine::Role::Viewer},
		{webengine::http::verb::delete_, "/api/v1/data-logging/artifacts",
		 webengine::Role::Admin},
		{webengine::http::verb::get, "/api/v1/data-logging/artifact",
		 webengine::Role::Viewer},
		{webengine::http::verb::get,
		 "/api/v1/data-logging/artifacts/preview", webengine::Role::Viewer},
		{webengine::http::verb::post,
		 "/api/v1/data-logging/artifacts/retry", webengine::Role::Admin},
		{webengine::http::verb::post,
		 "/api/v1/data-logging/channels/test", webengine::Role::Admin},
		{webengine::http::verb::get,
		 "/api/v1/data-logging/channel-materials", webengine::Role::Admin},
		{webengine::http::verb::put,
		 "/api/v1/data-logging/channel-credential", webengine::Role::Admin},
		{webengine::http::verb::delete_,
		 "/api/v1/data-logging/channel-credential", webengine::Role::Admin},
		{webengine::http::verb::delete_,
		 "/api/v1/data-logging/channel-asset", webengine::Role::Admin},
	});

consteval bool route_table_is_unique()
{
	for (std::size_t left = 0; left < route_table.size(); ++left)
		for (std::size_t right = left + 1; right < route_table.size(); ++right)
			if (route_table[left].method == route_table[right].method &&
			    route_table[left].path == route_table[right].path)
				return false;
	return true;
}

consteval bool m19_routes_have_required_roles()
{
	for (const auto &required : m19_route_contract) {
		bool found = false;
		for (const auto &route : route_table)
			if (route.method == required.method && route.path == required.path &&
			    route.min_role == required.role) {
				found = true;
				break;
			}
		if (!found)
			return false;
	}
	return true;
}

static_assert(route_table_is_unique(),
	"external API method/path pairs must be unique");
static_assert(m19_routes_have_required_roles(),
	"M19 external API role contract changed");

inline constexpr auto data_logging_download_role = webengine::Role::Viewer;
inline constexpr auto data_logging_asset_upload_role = webengine::Role::Admin;

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

	engine.add_streaming_download("/api/v1/waveforms/export",
		[&context](const auto &request) {
			return export_waveform_event(context, request);
		}, webengine::Role::Viewer);
	engine.add_streaming_download("/api/v1/data-logging/artifacts/download",
		[&context](const auto &request) {
			return download_data_logging_artifact(context, request);
		}, data_logging_download_role);

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
	const std::vector<std::string> data_channel_asset_types{
		"application/octet-stream", "application/x-pem-file",
		"application/pkix-cert", "application/x-x509-ca-cert",
		"text/plain"};
	engine.add_file_upload("/api/v1/data-logging/channel-asset",
		[&context](const auto &request, const auto &file) {
			return upload_data_logging_asset(context, request, file);
		}, data_logging_asset_upload_role, certificate_limit,
		data_channel_asset_types);

	engine.add_file_download("/api/v1/documentation/msap1_api.yaml",
		[&context](const auto &request) {
			return download_openapi_document(context, request);
		}, webengine::Role::Viewer);
	engine.add_file_download(
		"/api/v1/documentation/msap1_modbus_registers.xlsx",
		[&context](const auto &request) {
			return download_modbus_document(context, request);
		}, webengine::Role::Viewer);
}

} // namespace msap1::web::api
