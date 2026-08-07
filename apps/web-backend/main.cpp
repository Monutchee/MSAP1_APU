/**
 * @file main.cpp
 * @brief Process bootstrap for the MSAP1 web backend.
 *
 * This file owns process concerns only: service lifecycle, WebEngine and
 * nginx supervision, and wiring the shared AppContext into the API route
 * table.  The HTTP endpoints themselves live under api/ — see
 * api/routes.hpp for the complete route table.
 */

#include "api/routes.hpp"
#include "app_context.hpp"
#include "auth/msap1_auth_provider.hpp"
#include "mnc/logging/logging.hpp"
#include "mnc/service/service.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <memory>
#include <source_location>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <webengine/NginxController.hpp>
#include <webengine/WebEngine.hpp>

namespace {

using namespace std::chrono_literals;

constexpr const char *web_socket_path = "/run/monutchee/web-backend.sock";
constexpr const char *nginx_config_path = "/etc/monutchee/msap1/nginx.conf";
constexpr const char *nginx_listen_path = "/run/monutchee/nginx/listen.conf";
constexpr const char *nginx_pid_path = "/run/monutchee/nginx/nginx.pid";
constexpr const char *nginx_temp_path = "/run/monutchee/nginx";

const mnc::logging::Logger lifecycle_log("web-backend", "lifecycle");
const mnc::logging::Logger nginx_log("web-backend", "nginx");

void log_message(
	const mnc::logging::Logger &logger, mnc::logging::Priority priority,
	std::string message, std::string_view event,
	std::initializer_list<mnc::logging::Field> fields = {},
	const std::source_location &source = std::source_location::current())
{
	(void)logger.write(priority, message, event,
			   std::span<const mnc::logging::Field>(
				   fields.begin(), fields.size()),
			   source);
}

std::string getenv_or(const char *name, const char *fallback)
{
	const char *value = std::getenv(name);
	return value != nullptr && value[0] != '\0' ? value : fallback;
}

std::uint16_t web_port(const char *environment_name, const char *fallback)
{
	const auto value = getenv_or(environment_name, fallback);
	const auto port = std::stoul(value);
	if (port == 0 || port > std::numeric_limits<std::uint16_t>::max())
		throw std::invalid_argument(std::string(environment_name) +
			" is out of range");
	return static_cast<std::uint16_t>(port);
}

bool wait_for_socket(const std::filesystem::path &path,
		     const std::atomic<bool> &finished)
{
	const auto deadline = std::chrono::steady_clock::now() + 5s;
	while (std::chrono::steady_clock::now() < deadline && !finished) {
		std::error_code error;
		if (std::filesystem::exists(path, error))
			return true;
		std::this_thread::sleep_for(20ms);
	}
	return false;
}

std::atomic<webengine::WebEngine *> active_web_engine{nullptr};

class ActiveEngineRegistration {
public:
	explicit ActiveEngineRegistration(webengine::WebEngine &engine)
	{
		active_web_engine = &engine;
	}
	~ActiveEngineRegistration() { active_web_engine = nullptr; }
};

} // namespace

int run_web_backend()
{
	try {
		log_message(lifecycle_log, mnc::logging::Priority::notice,
			"MSAP1 web backend is starting", "service_starting");
		auto auth = std::make_shared<msap1::web::Msap1AuthProvider>();

		webengine::NginxController::Options nginx_options;
		nginx_options.config = getenv_or("MSAP1_NGINX_CONFIG", nginx_config_path);
		nginx_options.listen_file =
			getenv_or("MSAP1_NGINX_LISTEN_FILE", nginx_listen_path);
		nginx_options.pidfile = getenv_or("MSAP1_NGINX_PIDFILE", nginx_pid_path);
		nginx_options.temp_root =
			getenv_or("MSAP1_NGINX_TEMP_ROOT", nginx_temp_path);
		nginx_options.http_port = web_port("MSAP1_WEB_HTTP_PORT", "80");
		nginx_options.https_port = web_port("MSAP1_WEB_HTTPS_PORT", "443");
		nginx_options.https_enabled = true;
		webengine::NginxController nginx(std::move(nginx_options));

		/* Each gateway owns one persistent, correlation-aware AF_UNIX
		 * stream. HTTP handlers express typed product operations and
		 * never construct IPC frames. The context (and therefore the
		 * gateways) must outlive the engine that serves the routes. */
		msap1::web::AcquisitionGateway acquisition;
		msap1::web::SettingsGateway settings;
		msap1::web::AppContext context{acquisition, settings, nginx};

		webengine::WebEngine engine(auth);
		ActiveEngineRegistration active_engine(engine);
		engine.set_socket_path(getenv_or("MSAP1_WEB_SOCKET", web_socket_path))
			.set_threads(2)
			.enable_signal_shutdown()
			.enable_auth_endpoints()
			.protect_path("/protected/waveforms/",
				      webengine::Role::Viewer);

		msap1::web::api::register_routes(engine, context);

		std::atomic<bool> engine_finished{false};
		std::exception_ptr engine_error;
		std::thread engine_thread([&] {
			try {
				engine.run();
			} catch (...) {
				engine_error = std::current_exception();
			}
			engine_finished = true;
		});

		const auto socket_path = getenv_or("MSAP1_WEB_SOCKET", web_socket_path);
		if (!wait_for_socket(socket_path, engine_finished)) {
			engine.stop();
			engine_thread.join();
			throw std::runtime_error("WebEngine did not create its Unix socket");
		}
		log_message(lifecycle_log, mnc::logging::Priority::info,
			"WebEngine Unix socket is ready: " + socket_path,
			"backend_socket_ready",
			{{"MNC_SOCKET_PATH", socket_path}});
		if (!nginx.on()) {
			engine.stop();
			engine_thread.join();
			throw std::runtime_error("nginx failed to start: " + nginx.last_error());
		}
		log_message(nginx_log, mnc::logging::Priority::notice,
			"nginx started under web-backend supervision",
			"nginx_started");

		log_message(lifecycle_log, mnc::logging::Priority::notice,
			"MSAP1 web backend and nginx are ready", "service_ready");
		unsigned recovery_failures = 0;
		while (!engine_finished) {
			std::this_thread::sleep_for(5s);
			if (engine_finished)
				break;
			if (!nginx.is_running()) {
				log_message(nginx_log, mnc::logging::Priority::warning,
					"nginx stopped unexpectedly; recovery is starting",
					"nginx_unexpected_exit",
					{{"MNC_RECOVERY_ATTEMPT",
					  std::to_string(recovery_failures + 1)}});
				if (!nginx.on()) {
					if (++recovery_failures >= 3) {
						log_message(nginx_log,
							mnc::logging::Priority::critical,
							"nginx recovery failed repeatedly: " +
								nginx.last_error(),
							"nginx_recovery_exhausted");
						engine.stop();
						break;
					}
					continue;
				}
				recovery_failures = 0;
				log_message(nginx_log, mnc::logging::Priority::notice,
					"nginx recovered successfully",
					"nginx_recovered");
			}
		}

		log_message(lifecycle_log, mnc::logging::Priority::notice,
			"MSAP1 web backend is stopping", "service_stopping");
		(void)nginx.off();
		log_message(nginx_log, mnc::logging::Priority::info,
			"nginx stopped", "nginx_stopped");
		engine.stop();
		engine_thread.join();
		if (engine_error)
			std::rethrow_exception(engine_error);
		if (recovery_failures >= 3)
			throw std::runtime_error("nginx recovery failed repeatedly: " +
				nginx.last_error());
		return 0;
	} catch (const std::exception &error) {
		log_message(lifecycle_log, mnc::logging::Priority::critical,
			"msap1-web-backend: " + std::string(error.what()),
			"service_failed");
		return 1;
	}
}

namespace {

class WebBackendService final : public mnc::Service {
public:
	WebBackendService()
		: Service("MSAP1 web backend", "web-backend")
	{
	}

protected:
	void on_start() override
	{
		worker_ = std::thread([this] {
			const int result = run_web_backend();
			failed_ = result != 0;
			request_stop();
		});
	}

	void on_reload() override
	{
		/* WebEngine owns nginx configuration. A reload request is handled by
		 * its normal stop/start lifecycle so no partially updated listener is
		 * exposed. */
		(void)logger().write(mnc::logging::Priority::notice,
			"web backend reload requested", "reload_requested");
	}

	void on_stop() noexcept override
	{
		if (auto *engine = active_web_engine.load())
			engine->stop();
		if (worker_.joinable())
			worker_.join();
	}

	[[nodiscard]] mnc::ServiceHealth health() const override
	{
		return failed_.load()
			? mnc::ServiceHealth{false, "web backend worker failed"}
			: mnc::ServiceHealth{true, "web backend running"};
	}

private:
	std::thread worker_;
	std::atomic<bool> failed_{false};
};

} // namespace

int main()
{
	WebBackendService service;
	return service.execute();
}
