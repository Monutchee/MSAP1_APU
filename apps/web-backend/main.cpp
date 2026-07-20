#include "msap1_auth_provider.hpp"
#include "systemd_notifier.hpp"

#include "msap1/acquisition_ipc.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include <glaze/glaze.hpp>
#include <webengine/NginxController.hpp>
#include <webengine/WebEngine.hpp>

namespace {

using namespace std::chrono_literals;

constexpr const char *web_socket_path = "/run/monutchee/web-backend.sock";
constexpr const char *nginx_config_path = "/etc/monutchee/msap1/nginx.conf";
constexpr const char *nginx_listen_path = "/run/monutchee/nginx/listen.conf";
constexpr const char *nginx_pid_path = "/run/monutchee/nginx/nginx.pid";
constexpr const char *nginx_temp_path = "/run/monutchee/nginx";

struct SessionDto {
	std::string username;
	std::string role;
};

struct AcquisitionHealthDto {
	bool running;
	bool record_available;
	std::uint64_t records;
	std::uint64_t bytes;
	std::uint64_t read_errors;
	std::uint64_t invalid_records;
	std::uint64_t sequence_gaps;
	std::uint32_t configuration_generation;
};

struct AdcHealthDto {
	bool healthy;
	bool spi_responsive;
	bool initialized;
	bool configuration_match;
	bool capture_active;
	bool fifo_ok;
	bool headers_valid;
	bool meter_configured;
	bool meter_generation_match;
	bool dc_offset_removal;
	std::uint32_t sample_rate_hz;
	std::uint32_t frames;
	std::uint32_t fifo_overflows;
	std::uint32_t header_errors;
};

struct HealthDto {
	bool healthy;
	AcquisitionHealthDto acquisition;
	AdcHealthDto adc;
	bool backend_running;
	bool nginx_running;
};

struct MeterChannelDto {
	std::uint32_t index;
	std::string name;
	std::string unit;
	bool valid;
	std::int64_t mean_micro_units;
	std::uint32_t rms_count;
	double rms;
};

struct MeterReadingsDto {
	std::uint32_t sequence;
	std::uint32_t configuration_generation;
	std::uint32_t sample_rate_hz;
	std::uint32_t rms_window_samples;
	std::uint32_t status;
	std::uint32_t capture_frames;
	std::uint32_t header_errors;
	std::uint32_t fifo_overflows;
	std::uint32_t packetizer_drops;
	std::uint32_t hub_drops;
	std::array<MeterChannelDto, 8> channels;
};

bool health_flag(const msap1_adc_health_payload &health, std::uint32_t flag)
{
	return (health.health_flags & flag) != 0u;
}

bool meter_flag(const msap1_adc_health_payload &health, std::uint32_t flag)
{
	return (health.meter_health_flags & flag) != 0u;
}

template <typename T>
webengine::Response json_response(webengine::http::status status,
				  const T &value)
{
	auto body = glz::write_json(value);
	if (!body)
		return webengine::json(webengine::http::status::internal_server_error,
			R"({"error":"JSON serialization failed"})");
	return webengine::json(status, std::move(*body));
}

webengine::Response error_response(webengine::http::status status,
				   std::string message)
{
	return json_response(status, glz::obj{"error", std::move(message)});
}

void require_acquisition_ok(const msap1::AcquisitionResponse &response)
{
	if (response.status != msap1::AcquisitionStatus::ok)
		throw std::runtime_error("acquisition daemon returned status " +
			std::to_string(static_cast<std::uint32_t>(response.status)));
}

HealthDto health(const msap1::AcquisitionResponse &response,
		 const webengine::NginxController &nginx)
{
	const auto &adc = response.rpu_health;
	const bool spi = health_flag(adc, MSAP1_ADC_HEALTH_SPI_RESPONSIVE);
	const bool initialized = health_flag(adc, MSAP1_ADC_HEALTH_INITIALIZED) &&
		health_flag(adc, MSAP1_ADC_HEALTH_INIT_COMPLETE);
	const bool config = health_flag(adc, MSAP1_ADC_HEALTH_CONFIG_MATCH);
	const bool active = health_flag(adc, MSAP1_ADC_HEALTH_CAPTURE_ACTIVE);
	const bool fifo = health_flag(adc, MSAP1_ADC_HEALTH_NO_OVERFLOW);
	const bool headers = health_flag(adc, MSAP1_ADC_HEALTH_HEADERS_VALID);
	const bool meter_configured =
		meter_flag(adc, MSAP1_METER_HEALTH_CORES_PRESENT) &&
		meter_flag(adc, MSAP1_METER_HEALTH_CONFIGURED) &&
		meter_flag(adc, MSAP1_METER_HEALTH_ENABLED);
	const bool generation =
		meter_flag(adc, MSAP1_METER_HEALTH_GENERATION_MATCH) &&
		adc.meter_generation == response.configuration_generation;
	const bool dc = meter_flag(adc, MSAP1_METER_HEALTH_REMOVE_DC);
	const bool acquisition_ok = response.running != 0u &&
		response.has_meter_record != 0u && response.dma_read_errors == 0u &&
		response.invalid_records == 0u && response.sequence_gaps == 0u;
	const bool adc_ok = spi && initialized && config && active && fifo &&
		headers && meter_configured && generation && dc;
	const bool nginx_ok = nginx.is_running();
	return {
		acquisition_ok && adc_ok && nginx_ok,
		{response.running != 0u, response.has_meter_record != 0u,
		 response.meter_records, response.dma_bytes, response.dma_read_errors,
		 response.invalid_records, response.sequence_gaps,
		 response.configuration_generation},
		{adc_ok, spi, initialized, config, active, fifo, headers,
		 meter_configured, generation, dc, adc.sample_rate_hz,
		 adc.frame_count, adc.overflow_count, adc.header_error_count},
		true,
		nginx_ok,
	};
}

MeterReadingsDto readings(const msap1::AcquisitionResponse &response)
{
	if (response.running == 0u || response.has_meter_record == 0u)
		throw std::runtime_error("no meter result is available");
	const auto &record = response.latest_record;
	if (!record.header_valid())
		throw std::runtime_error("meter record header is invalid");
	static constexpr std::array<const char *, 8> names{
		"ILA", "ILB", "ILC", "ILN", "VLC", "VLB", "VLA", "VCM"};
	MeterReadingsDto result{
		record.sequence(), record.configuration_generation(),
		record.sample_rate_hz(), record.window_samples(), record.status(),
		record.capture_frames(), record.header_errors(),
		record.fifo_overflows(), record.packetizer_drops(), record.hub_drops(),
		{},
	};
	for (std::size_t index = 0; index < result.channels.size(); ++index) {
		const auto reading = record.channel(index);
		result.channels[index] = {
			static_cast<std::uint32_t>(index), names[index],
			index >= 4 && index <= 6 ? "V" : "A", reading.valid,
			reading.mean_micro_units, reading.rms_count,
			reading.valid
				? static_cast<double>(reading.rms_micro_units) / 1000000.0
				: 0.0,
		};
	}
	return result;
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

} // namespace

int main()
{
	try {
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

		webengine::WebEngine engine(auth);
		engine.set_socket_path(getenv_or("MSAP1_WEB_SOCKET", web_socket_path))
			.set_threads(2)
			.enable_signal_shutdown()
			.enable_auth_endpoints();

		engine.add_api(webengine::http::verb::get, "/api/v1/session",
			[](const webengine::RequestContext &context) {
				const auto &user = *context.user;
				return json_response(webengine::http::status::ok,
					SessionDto{user.username,
						   webengine::role_name(user.role)});
			}, webengine::Role::Viewer);

		engine.add_api(webengine::http::verb::get, "/api/v1/health",
			[&nginx](const webengine::RequestContext &) {
				try {
					msap1::AcquisitionClient client;
					const auto response = client.request(
						msap1::AcquisitionCommand::health, 1000);
					require_acquisition_ok(response);
					return json_response(webengine::http::status::ok,
						health(response, nginx));
				} catch (const std::exception &error) {
					return error_response(
						webengine::http::status::service_unavailable,
						error.what());
				}
			}, webengine::Role::Viewer);

		engine.add_api(webengine::http::verb::get,
			"/api/v1/meter/readings",
			[](const webengine::RequestContext &) {
				try {
					msap1::AcquisitionClient client;
					const auto response = client.request(
						msap1::AcquisitionCommand::info, 1000);
					require_acquisition_ok(response);
					return json_response(webengine::http::status::ok,
						readings(response));
				} catch (const std::exception &error) {
					return error_response(
						webengine::http::status::service_unavailable,
						error.what());
				}
			}, webengine::Role::Viewer);

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
		if (!nginx.on()) {
			engine.stop();
			engine_thread.join();
			throw std::runtime_error("nginx failed to start: " + nginx.last_error());
		}

		msap1::web::SystemdNotifier notifier;
		(void)notifier.ready("MSAP1 web backend and nginx are ready");
		unsigned recovery_failures = 0;
		while (!engine_finished) {
			std::this_thread::sleep_for(5s);
			if (engine_finished)
				break;
			if (!nginx.is_running()) {
				if (!nginx.on()) {
					if (++recovery_failures >= 3) {
						engine.stop();
						break;
					}
					continue;
				}
				recovery_failures = 0;
			}
			(void)notifier.watchdog("MSAP1 web backend and nginx are healthy");
		}

		(void)notifier.stopping("MSAP1 web backend is stopping");
		(void)nginx.off();
		engine.stop();
		engine_thread.join();
		if (engine_error)
			std::rethrow_exception(engine_error);
		if (recovery_failures >= 3)
			throw std::runtime_error("nginx recovery failed repeatedly: " +
				nginx.last_error());
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "msap1-web-backend: " << error.what() << '\n';
		return 1;
	}
}
