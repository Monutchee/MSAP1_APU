#include "manager_daemon.hpp"

#include "product_units.hpp"
#include "msap1/settings/settings_ipc.hpp"

#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <sys/stat.h>

namespace msap1::service_manager::daemon {

using namespace std::chrono_literals;

ServiceManagerDaemon::ServiceManagerDaemon()
	: Service("MSAP1 service manager", "service-manager"),
	  manager_(mnc::make_systemd_unit_controller()),
	  router_(manager_),
	  server_(context_.get_executor(),
		  std::string(msap1::service_control::socket_path)),
	  auditor_(context_, manager_, logger()),
	  settings_policy_timer_(context_.get_executor())
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
	reconcile_settings_policy();
	schedule_settings_policy();
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
	reconcile_settings_policy();
	auditor_.run_once();
}

void ServiceManagerDaemon::on_stop() noexcept
{
	settings_policy_timer_.cancel();
	server_.stop();
	context_.stop();
	if (worker_.joinable())
		worker_.join();
}

void ServiceManagerDaemon::schedule_settings_policy()
{
	settings_policy_timer_.expires_after(2s);
	settings_policy_timer_.async_wait(
		[this](const boost::system::error_code &error) {
			if (error)
				return;
			reconcile_settings_policy();
			schedule_settings_policy();
		});
}

void ServiceManagerDaemon::reconcile_settings_policy()
{
	try {
		const auto settings =
			msap1::settings::ipc::SettingsClient{}.active(1500);
		const auto &mqtt = settings.mqtt;
		const auto status = manager_.status("mqtt-publisher");
		const auto running = status.active_state == "active" ||
			status.active_state == "activating";
		if (mqtt.enabled && !running) {
			(void)manager_.control("mqtt-publisher", mnc::ServiceAction::start);
			(void)logger().write(mnc::logging::Priority::notice,
				"started MQTT publisher from active settings",
				"settings_controlled_service_started");
		} else if (!mqtt.enabled && running) {
			(void)manager_.control("mqtt-publisher", mnc::ServiceAction::stop);
			(void)logger().write(mnc::logging::Priority::notice,
				"stopped MQTT publisher from active settings",
				"settings_controlled_service_stopped");
		}

		const auto service_running = [this](std::string_view service) {
			const auto state = manager_.status(service).active_state;
			return state == "active" || state == "activating";
		};
		const auto set_running = [this, &service_running](std::string_view service,
			bool desired) {
			if (service_running(service) == desired)
				return;
			(void)manager_.control(service, desired
				? mnc::ServiceAction::start : mnc::ServiceAction::stop);
			(void)logger().write(mnc::logging::Priority::notice,
				std::string(desired ? "started " : "stopped ") +
					std::string(service) + " from active settings",
				"time_sync_policy_applied");
		};
		if (settings.time.synchronization == "ptp") {
			set_running("time-sync-ntp", false);
			set_running("time-sync-ptp-clock", true);
			set_running("time-sync-ptp-system", true);
		} else {
			set_running("time-sync-ptp-system", false);
			set_running("time-sync-ptp-clock", false);
			set_running("time-sync-ntp", true);
		}
	} catch (const std::exception &error) {
		(void)logger().write(mnc::logging::Priority::debug,
			"settings-controlled service policy deferred: " +
				std::string(error.what()),
			"settings_policy_deferred");
	}
}

mnc::ServiceHealth ServiceManagerDaemon::health() const
{
	if (failed_)
		return {false, "service-manager worker failed"};
	return {true, auditor_.degraded() ? "managed service is degraded"
					  : "managed services are healthy"};
}

} // namespace msap1::service_manager::daemon
