#pragma once

/**
 * @file settings_apply.hpp
 * @brief Bridge that hot-applies a settings snapshot to the acquisition
 *        daemon.
 */

#include "msap1/settings/settings.hpp"

namespace msap1::settings::daemon {

/**
 * @brief Send one complete settings snapshot to the acquisition daemon.
 *
 * Called by the SettingsHandler's apply coordinator inside every save and
 * factory reset, BEFORE the document is persisted: a snapshot the running
 * pipeline rejects must never become the stored active document.
 *
 * @throws std::runtime_error when acquisition is unreachable or rejects
 *         the snapshot (mapped to Status::apply_failed on the wire).
 */
void apply_to_acquisition(const msap1::settings::ProductSettings &settings);

/** Apply database routing/retention to the stream and historian services. */
void apply_to_database_services(
	const msap1::settings::ProductSettings &settings);

/** Reconcile the optional MQTT unit with the candidate settings. */
void apply_to_mqtt_service(const msap1::settings::ProductSettings &settings);

/** Apply one complete product snapshot to every runtime settings consumer. */
void apply_to_runtime(const msap1::settings::ProductSettings &settings);

} // namespace msap1::settings::daemon
