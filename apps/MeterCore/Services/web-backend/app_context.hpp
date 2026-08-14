#pragma once

/**
 * @file app_context.hpp
 * @brief Shared service dependencies handed to every HTTP route handler.
 */

#include "gateway/acquisition_gateway.hpp"
#include "gateway/database_gateway.hpp"
#include "gateway/mqtt_gateway.hpp"
#include "gateway/settings_gateway.hpp"

namespace webengine {
class NginxController;
}

namespace msap1::web {

/**
 * @brief Aggregates the long-lived services an HTTP handler may use.
 *
 * run_web_backend() constructs one AppContext once the gateways and the
 * nginx controller exist and keeps it alive for the whole WebEngine
 * lifetime.  Route handlers receive it by reference (see api/routes.hpp),
 * so they carry no global state and can be exercised with substitute
 * dependencies in tests.
 */
struct AppContext {
	/** Typed IPC boundary to the acquisition daemon. */
	AcquisitionGateway &acquisition;
	/** Typed IPC boundary to the persistent settings authority. */
	SettingsGateway &settings;
	/** Typed IPC boundary to durable stream and historian services. */
	DatabaseGateway &database;
	/** Typed boundary to MQTT runtime state and protected assets. */
	MqttGateway &mqtt;
	/** Supervised nginx front end; consulted by the system health API. */
	webengine::NginxController &nginx;
};

} // namespace msap1::web
