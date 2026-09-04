#pragma once

/**
 * @file product_units.hpp
 * @brief The ordered registry of managed MSAP1 product services.
 */

#include "mnc/service/service_manager.hpp"

namespace msap1::service_manager::daemon {

/**
 * @brief Register every managed product unit with its start dependencies.
 *
 * This is the single place that defines the product's service topology.
 * Order matters: each service lists the services that must be running
 * before it starts, and start_registered() honors those edges:
 *
 *   settings  ->  meter-stream  ->  fpga-acquisition  ->  web-backend
 *                         \----->  meter-historian  ----/
 *                         \----->  mqtt-publisher (settings-controlled)
 *                                   meter-historian -> data-sender
 *                         \----->  waveform-converter
 *   settings -> time-sync-ntp OR time-sync-ptp-clock -> time-sync-ptp-system
 *
 * (Acquisition needs the settings authority to hand it the active
 * configuration; the web backend needs acquisition for live data.)
 * systemd remains the only process supervisor and restart-policy owner —
 * the manager orders and adopts units through sd-bus, it does not respawn
 * them itself.
 */
inline void register_product_units(mnc::ServiceManager &manager)
{
	manager.register_service({"settings",
		"msap1-settings.service", {}});
	/* Observe the manager's own priority without asking a Type=notify service
	 * to start itself while it is still activating. */
	manager.register_service({"service-manager",
		"msap1-service-manager.service", {"settings"}, false});
	manager.register_service({"meter-stream",
		"msap1-meter-stream.service", {"settings"}, true,
		mnc::ServicePriorityTier::high});
	manager.register_service({"fpga-acquisition",
		"msap1-fpga-acquisition.service", {"settings", "meter-stream"}, true,
		mnc::ServicePriorityTier::critical});
	manager.register_service({"meter-historian",
		"msap1-meter-historian.service", {"meter-stream"}, true,
		mnc::ServicePriorityTier::background});
	manager.register_service({"modbus",
		"msap1-modbus-server.service",
		{"settings", "fpga-acquisition"}});
	manager.register_service({"mqtt-publisher",
		"msap1-mqtt-publisher.service",
		{"settings", "fpga-acquisition"}, false,
		mnc::ServicePriorityTier::background});
	manager.register_service({"time-sync-ntp",
		"systemd-timesyncd.service", {"settings"}, false});
	manager.register_service({"time-sync-ptp-clock",
		"ptp4l@end0.service", {"settings"}, false});
	manager.register_service({"time-sync-ptp-system",
		"phc2sys@end0.service", {"time-sync-ptp-clock"}, false});
	manager.register_service({"data-sender",
		"msap1-data-sender.service", {"settings", "meter-historian"}, true,
		mnc::ServicePriorityTier::background});
	manager.register_service({"waveform-converter",
		"msap1-waveform-converter.service", {"fpga-acquisition"}, true,
		mnc::ServicePriorityTier::background});
	manager.register_service({"web-backend",
		"msap1-web-backend.service",
		{"fpga-acquisition", "meter-historian", "data-sender"}});
}

} // namespace msap1::service_manager::daemon
