#include "settings_daemon.hpp"

#include "apply/settings_apply.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>

#include <sys/stat.h>

namespace msap1::settings::daemon {

SettingsDaemon::SettingsDaemon()
	: Service("MSAP1 settings", "settings"),
	  handler_(std::filesystem::path(msap1::settings::persistent_root),
		   std::filesystem::path(
			   msap1::settings::factory_defaults_path),
		   msap1::settings::SettingsApplyCoordinator{
			   [](const auto &settings) {
				   apply_to_runtime(settings);
			   },
			   [](const auto &settings) {
				   apply_to_mqtt_service(settings);
				   apply_to_data_sender_service(settings);
			   }}),
	  router_(handler_),
	  server_(context_.get_executor(),
		  std::string(msap1::settings::socket_path))
{
}

void SettingsDaemon::on_start()
{
	handler_.initialize();
	if (handler_.recovery_mode()) {
		(void)logger().write(mnc::logging::Priority::warning,
			"settings authority entered recovery mode: " +
				handler_.recovery_reason(),
			"settings_recovery_mode");
	} else {
		(void)logger().write(mnc::logging::Priority::notice,
			"settings authority initialized", "settings_ready");
	}
	server_.start(
		[this](auto connection, auto frame) {
			router_.handle(std::move(connection), std::move(frame));
		},
		[this](const std::string &message) {
			(void)logger().write(mnc::logging::Priority::warning,
				"settings IPC error: " + message, "ipc_error");
		});
	/* Group-restricted socket: the access policy in access_policy.hpp
	 * depends on peers having to be root or in the authority group. */
	if (::chmod(msap1::settings::socket_path.data(), 0660) != 0)
		throw std::runtime_error("cannot set settings socket mode");
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

void SettingsDaemon::on_stop() noexcept
{
	server_.stop();
	context_.stop();
	if (worker_.joinable())
		worker_.join();
}

mnc::ServiceHealth SettingsDaemon::health() const
{
	if (failed_)
		return {false, "settings worker failed"};
	if (handler_.recovery_mode())
		return {true, "settings recovery mode: " +
				      handler_.recovery_reason()};
	return {true, "settings authority ready"};
}

} // namespace msap1::settings::daemon
