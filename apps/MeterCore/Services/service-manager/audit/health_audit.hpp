#pragma once

/**
 * @file health_audit.hpp
 * @brief Periodic degraded-state audit of the managed product services.
 */

#include "mnc/logging/logging.hpp"
#include "mnc/service/service_manager.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <atomic>

namespace msap1::service_manager::daemon {

/**
 * @brief Watches the managed units and maintains the daemon's degraded flag.
 *
 * Every five seconds the auditor asks the ServiceManager for each managed
 * unit's status. A required unit that is not active, or any unit that systemd has given up
 * restarting, marks the product degraded — the flag feeds the daemon's own
 * mnc::Service health, which is what the platform watchdog observes. A
 * unit that exhausted its restart policy is additionally logged at
 * critical priority with its restart count.
 *
 * The audit runs on the daemon's Asio thread; degraded() is an atomic read
 * so the service health callback may call it from any thread.
 */
class HealthAuditor final {
public:
	/**
	 * @param context Asio context whose thread runs the audits.
	 * @param manager The unit manager to query; must outlive the auditor.
	 * @param logger  The daemon's service logger for audit findings.
	 */
	HealthAuditor(boost::asio::io_context &context,
		      mnc::ServiceManager &manager,
		      const mnc::logging::Logger &logger);

	/** @brief Start the five-second audit cycle. */
	void start();

	/** @brief Run one audit immediately (used by service reload). */
	void run_once();

	/** @brief True when any managed unit is inactive or failed. */
	[[nodiscard]] bool degraded() const { return degraded_.load(); }

private:
	void schedule();

	mnc::ServiceManager &manager_;
	const mnc::logging::Logger &logger_;
	boost::asio::steady_timer timer_;
	std::atomic<bool> degraded_{false};
};

} // namespace msap1::service_manager::daemon
