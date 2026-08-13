#pragma once

/**
 * @file manager_daemon.hpp
 * @brief systemd service shell of the MSAP1 service manager.
 */

#include "audit/health_audit.hpp"
#include "ipc/request_router.hpp"
#include "mnc/ipc/ipc.hpp"
#include "mnc/service/service.hpp"
#include "mnc/service/service_manager.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <atomic>
#include <exception>
#include <thread>

namespace msap1::service_manager::daemon {

/**
 * @brief Wires the service manager together and runs its lifecycle.
 *
 * Composition: the mnc::ServiceManager orders and adopts the product units
 * through sd-bus (product_units.hpp defines which units and in what
 * order); the RequestRouter serves the control socket; the HealthAuditor
 * keeps the degraded flag current. This class owns the Unix socket server
 * and the single Asio worker thread everything above runs on, and reports
 * worker failure or product degradation through the mnc::Service health
 * interface.
 */
class ServiceManagerDaemon final : public mnc::Service {
public:
	ServiceManagerDaemon();

protected:
	/** @brief Start units in order, serve the socket, begin auditing. */
	void on_start() override;
	/** @brief Re-assert unit ordering and audit immediately. */
	void on_reload() override;
	/** @brief Stop the server and join the worker thread. */
	void on_stop() noexcept override;
	/** @brief Reports worker failure or a degraded managed service. */
	[[nodiscard]] mnc::ServiceHealth health() const override;

private:
	void schedule_settings_policy();
	void reconcile_settings_policy();

	mnc::ServiceManager manager_;
	RequestRouter router_;
	boost::asio::io_context context_;
	mnc::ipc::UnixStreamServer server_;
	HealthAuditor auditor_;
	boost::asio::steady_timer settings_policy_timer_;
	std::thread worker_;
	std::exception_ptr failure_;
	std::atomic<bool> failed_{false};
};

} // namespace msap1::service_manager::daemon
