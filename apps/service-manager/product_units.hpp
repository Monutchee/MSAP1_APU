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
 *   settings  ->  fpga-acquisition  ->  web-backend
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
	manager.register_service({"fpga-acquisition",
		"msap1-fpga-acquisition.service", {"settings"}});
	manager.register_service({"web-backend",
		"msap1-web-backend.service", {"fpga-acquisition"}});
}

} // namespace msap1::service_manager::daemon
