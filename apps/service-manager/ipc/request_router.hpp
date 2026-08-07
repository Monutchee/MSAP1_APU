#pragma once

/**
 * @file request_router.hpp
 * @brief Decodes, authorizes, and dispatches service-control requests.
 */

#include "mnc/ipc/ipc.hpp"
#include "mnc/service/service_manager.hpp"
#include "msap1/service/service_control.hpp"

namespace msap1::service_manager::daemon {

/**
 * @brief The protocol layer of the service manager.
 *
 * For every incoming frame the router decodes the request, applies the
 * access policy, executes it against the ServiceManager, and sends exactly
 * one correlated response.
 *
 * Access policy: list/status are readable by every peer that can open the
 * socket (it is world-readable for diagnostics); the control actions
 * (start/stop/restart/reload) require uid 0, verified via SO_PEERCRED —
 * never via anything the client sends.
 *
 * The router runs on the daemon's single Asio worker thread, so it needs
 * no locking.
 */
class RequestRouter final {
public:
	/** @param manager The unit manager; must outlive the router. */
	explicit RequestRouter(mnc::ServiceManager &manager);

	/** @brief Serve one request frame and send its response. */
	void handle(mnc::ipc::UnixStreamServer::Connection connection,
		    mnc::ipc::Frame frame);

private:
	/** @brief True for the four mutating unit-control commands. */
	static bool is_control(msap1::service_control::Command command);
	/** @brief Map a control command onto the manager's action enum. */
	static mnc::ServiceAction
	action(msap1::service_control::Command command);

	mnc::ServiceManager &manager_;
};

} // namespace msap1::service_manager::daemon
