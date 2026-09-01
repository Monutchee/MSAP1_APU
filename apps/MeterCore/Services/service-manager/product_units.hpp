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
	manager.register_service({"meter-stream",
		"msap1-meter-stream.service", {"settings"}});
	manager.register_service({"fpga-acquisition",
		"msap1-fpga-acquisition.service", {"settings", "meter-stream"}});
	manager.register_service({"meter-historian",
		"msap1-meter-historian.service", {"meter-stream"}});
	manager.register_service({"modbus",
		"msap1-modbus-server.service",
		{"settings", "fpga-acquisition"}});
	manager.register_service({"mqtt-publisher",
		"msap1-mqtt-publisher.service",
		{"settings", "fpga-acquisition"}, false});
	manager.register_service({"data-sender",
		"msap1-data-sender.service", {"settings", "meter-historian"}});
	manager.register_service({"web-backend",
		"msap1-web-backend.service",
		{"fpga-acquisition", "meter-historian", "data-sender"}});
}

} // namespace msap1::service_manager::daemon
