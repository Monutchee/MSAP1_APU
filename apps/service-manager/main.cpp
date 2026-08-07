/**
 * @file main.cpp
 * @brief Entry point of msap1-service-manager.
 *
 * The daemon is decomposed by responsibility:
 *  - manager_daemon.*       — service shell: socket server, worker, health
 *  - product_units.hpp      — the ordered registry of managed product units
 *  - ipc/request_router.*   — decode -> authorize (root) -> dispatch
 *  - audit/health_audit.*   — periodic degraded-state audit of the units
 */

#include "manager_daemon.hpp"

int main()
{
	try {
		msap1::service_manager::daemon::ServiceManagerDaemon service;
		return service.execute();
	} catch (const std::exception &) {
		return 1;
	}
}
