#include "manager_daemon.hpp"

#include "product_units.hpp"

#include <stdexcept>
#include <string>
#include <utility>

#include <sys/stat.h>

namespace msap1::service_manager::daemon {

ServiceManagerDaemon::ServiceManagerDaemon()
	: Service("MSAP1 service manager", "service-manager"),
	  manager_(mnc::make_systemd_unit_controller()),
	  router_(manager_),
	  server_(context_.get_executor(),
		  std::string(msap1::service_control::socket_path)),
	  auditor_(context_, manager_, logger())
{
	register_product_units(manager_);
}

void ServiceManagerDaemon::on_start()
{
	manager_.start_registered();
	server_.start(
		[this](auto connection, auto frame) {
			router_.handle(std::move(connection), std::move(frame));
		},
		[this](const std::string &message) {
			(void)logger().write(mnc::logging::Priority::warning,
				"service-manager IPC error: " + message,
				"ipc_error");
		});
	/* Read-only operations are available to diagnostic users. Mutating
	 * requests are still authorized with SO_PEERCRED in the router. */
	if (::chmod(msap1::service_control::socket_path.data(), 0666) != 0)
		throw std::runtime_error(
			"cannot set service-manager socket mode");
	auditor_.start();
	worker_ = std::thread([this] {
		try {
			context_.run();
		} catch (...) {
			failure_ = std::current_exception();
			failed_ = true;
			request_stop();
		}
	});
}

void ServiceManagerDaemon::on_reload()
{
	manager_.start_registered();
	auditor_.run_once();
}

void ServiceManagerDaemon::on_stop() noexcept
{
	server_.stop();
	context_.stop();
	if (worker_.joinable())
		worker_.join();
}

mnc::ServiceHealth ServiceManagerDaemon::health() const
{
	if (failed_)
		return {false, "service-manager worker failed"};
	return {true, auditor_.degraded() ? "managed service is degraded"
					  : "managed services are healthy"};
}

} // namespace msap1::service_manager::daemon
