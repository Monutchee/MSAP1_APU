#include "msap1_auth_provider.hpp"
#include "systemd_notifier.hpp"

#include "msap1/acquisition_ipc.hpp"
#include "msap1/adc_sample.hpp"
#include "msap1/shared_ring.hpp"

#include <array>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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
	std::uint64_t frames;
	std::uint64_t bytes;
	std::uint64_t dma_blocks;
	std::uint64_t read_errors;
};

struct AdcHealthDto {
	bool healthy;
	bool spi_responsive;
	bool initialized;
	bool init_complete;
	bool configuration_match;
	bool capture_active;
	bool fifo_ok;
	bool headers_valid;
	std::uint32_t sample_rate_hz;
	std::uint32_t capture_flags;
	std::uint32_t frames;
	std::uint32_t packets;
	std::uint32_t fifo_overflows;
	std::uint32_t header_errors;
	std::uint32_t alerts;
	std::uint32_t spi_error;
};

struct WebHealthDto {
	bool backend_running;
	bool nginx_running;
};

struct HealthDto {
	bool healthy;
	AcquisitionHealthDto acquisition;
	AdcHealthDto adc;
	WebHealthDto web;
};

struct ChannelDto {
	std::uint32_t index;
	std::string name;
	std::string unit;
};

struct MetadataDto {
	std::uint32_t sample_rate_hz;
	std::uint32_t channel_count;
	std::uint32_t frame_size_bytes;
	std::uint32_t ring_capacity_frames;
	std::uint64_t published_sequence;
	std::uint32_t capture_flags;
	std::vector<ChannelDto> channels;
};

struct SampleDto {
	std::uint64_t sequence;
	std::array<std::int32_t, msap1::adc_channel_count> values;
};

struct SamplesDto {
	std::uint32_t capture_rate_hz;
	std::uint32_t display_rate_hz;
	std::uint64_t next_sequence;
	std::uint64_t dropped_frames;
	std::vector<SampleDto> frames;
};

bool health_flag(const msap1_adc_health_payload &health, std::uint32_t flag)
{
	return (health.health_flags & flag) != 0u;
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

std::map<std::string, std::string> parse_query(const webengine::Request &request)
{
	const auto target_view = request.target();
	const std::string target(target_view.data(), target_view.size());
	const auto query_begin = target.find('?');
	std::map<std::string, std::string> result;
	if (query_begin == std::string::npos)
		return result;

	std::size_t begin = query_begin + 1;
	while (begin < target.size()) {
		const auto end = target.find('&', begin);
		const auto item = target.substr(begin, end - begin);
		const auto separator = item.find('=');
		if (separator != std::string::npos && separator != 0)
			result[item.substr(0, separator)] = item.substr(separator + 1);
		if (end == std::string::npos)
			break;
		begin = end + 1;
	}
	return result;
}

std::uint64_t parse_unsigned(const std::map<std::string, std::string> &query,
			     std::string_view name, std::uint64_t fallback,
			     std::uint64_t maximum)
{
	const auto item = query.find(std::string(name));
	if (item == query.end())
		return fallback;
	if (item->second.empty() ||
	    !std::ranges::all_of(item->second, [](unsigned char character) {
		    return std::isdigit(character) != 0;
	    }))
		throw std::invalid_argument(std::string(name) + " must be an integer");
	std::size_t parsed = 0;
	std::uint64_t value = 0;
	try {
		value = std::stoull(item->second, &parsed, 10);
	} catch (const std::exception &) {
		throw std::invalid_argument(std::string(name) + " must be an integer");
	}
	if (parsed != item->second.size() || value > maximum)
		throw std::invalid_argument(std::string(name) + " is out of range");
	return value;
}

class SampleReader {
public:
	SamplesDto read(const webengine::Request &request)
	{
		std::lock_guard lock(mutex_);
		if (!ring_)
			ring_ = std::make_unique<msap1::SharedRingReader>();
		if (!ring_->running())
			throw std::runtime_error("FPGA acquisition is not running");

		const auto query = parse_query(request);
		const auto capture_rate = ring_->sample_rate_hz();
		const auto display_rate = static_cast<std::uint32_t>(
			parse_unsigned(query, "rate_hz", 20, capture_rate));
		const auto limit = static_cast<std::size_t>(
			parse_unsigned(query, "limit", 20, 256));
		if (display_rate == 0 || capture_rate % display_rate != 0)
			throw std::invalid_argument(
				"rate_hz must be a non-zero divisor of the capture rate");
		if (limit == 0)
			throw std::invalid_argument("limit must be between 1 and 256");

		const auto stride = capture_rate / display_rate;
		const auto published = ring_->published_sequence();
		std::uint64_t cursor = 0;
		if (query.contains("after")) {
			cursor = parse_unsigned(query, "after", 0,
						std::numeric_limits<std::uint64_t>::max());
		} else {
			const auto history = static_cast<std::uint64_t>(limit) * stride;
			cursor = published > history ? published - history : 0;
		}

		SamplesDto response{capture_rate, display_rate, cursor, 0, {}};
		response.frames.reserve(limit);
		while (response.frames.size() < limit) {
			msap1::AdcSampleFrame frame{};
			if (!ring_->read(cursor, frame, response.dropped_frames))
				break;
			const auto sequence = cursor - 1;
			response.frames.push_back({sequence, frame});
			cursor = sequence + stride;
		}
		response.next_sequence = cursor;
		return response;
	}

private:
	std::mutex mutex_;
	std::unique_ptr<msap1::SharedRingReader> ring_;
};

MetadataDto metadata(const msap1::AcquisitionResponse &response)
{
	static constexpr std::array<const char *, msap1::adc_channel_count> names{
		"ILA", "ILB", "ILC", "ILN", "VLC", "VLB", "VLA", "VCM"};
	MetadataDto result{
		response.sample_rate_hz,
		response.channel_count,
		response.frame_size,
		response.ring_capacity,
		response.published_sequence,
		response.capture_flags,
		{},
	};
	result.channels.reserve(names.size());
	for (std::size_t index = 0; index < names.size(); ++index)
		result.channels.push_back(
			{static_cast<std::uint32_t>(index), names[index], "raw_count"});
	return result;
}

HealthDto health(const msap1::AcquisitionResponse &response,
		 const webengine::NginxController &nginx)
{
	const auto &adc = response.rpu_health;
	const bool spi = health_flag(adc, MSAP1_ADC_HEALTH_SPI_RESPONSIVE);
	const bool initialized = health_flag(adc, MSAP1_ADC_HEALTH_INITIALIZED);
	const bool init_complete = health_flag(adc, MSAP1_ADC_HEALTH_INIT_COMPLETE);
	const bool config = health_flag(adc, MSAP1_ADC_HEALTH_CONFIG_MATCH);
	const bool active = health_flag(adc, MSAP1_ADC_HEALTH_CAPTURE_ACTIVE);
	const bool fifo = health_flag(adc, MSAP1_ADC_HEALTH_NO_OVERFLOW);
	const bool headers = health_flag(adc, MSAP1_ADC_HEALTH_HEADERS_VALID);
	const bool linux_ok = response.running != 0u &&
		response.published_sequence != 0u && response.iio_read_errors == 0u;
	const bool adc_ok = spi && initialized && init_complete && config && active &&
		fifo && headers;
	const bool nginx_ok = nginx.is_running();
	return {
		linux_ok && adc_ok && nginx_ok,
		{response.running != 0u, response.published_sequence,
		 response.iio_bytes, response.iio_blocks, response.iio_read_errors},
		{adc_ok, spi, initialized, init_complete, config, active, fifo, headers,
		 adc.sample_rate_hz, adc.capture_flags, adc.frame_count, adc.packet_count,
		 adc.overflow_count, adc.header_error_count, adc.alert_count,
		 adc.spi_error},
		{true, nginx_ok},
	};
}

void require_acquisition_ok(const msap1::AcquisitionResponse &response)
{
	if (response.status != msap1::AcquisitionStatus::ok)
		throw std::runtime_error("acquisition daemon returned status " +
			std::to_string(static_cast<std::uint32_t>(response.status)));
}

std::string getenv_or(const char *name, const char *fallback)
{
	const char *value = std::getenv(name);
	return value != nullptr && value[0] != '\0' ? value : fallback;
}

std::uint16_t http_port()
{
	const auto value = getenv_or("MSAP1_WEB_HTTP_PORT", "80");
	const auto port = std::stoul(value);
	if (port == 0 || port > std::numeric_limits<std::uint16_t>::max())
		throw std::invalid_argument("MSAP1_WEB_HTTP_PORT is out of range");
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
		auto sample_reader = std::make_shared<SampleReader>();

		webengine::NginxController::Options nginx_options;
		nginx_options.config = getenv_or("MSAP1_NGINX_CONFIG", nginx_config_path);
		nginx_options.listen_file =
			getenv_or("MSAP1_NGINX_LISTEN_FILE", nginx_listen_path);
		nginx_options.pidfile = getenv_or("MSAP1_NGINX_PIDFILE", nginx_pid_path);
		nginx_options.temp_root =
			getenv_or("MSAP1_NGINX_TEMP_ROOT", nginx_temp_path);
		nginx_options.http_port = http_port();
		nginx_options.https_enabled = false;
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

		engine.add_api(webengine::http::verb::get, "/api/v1/adc/metadata",
			[](const webengine::RequestContext &) {
				try {
					msap1::AcquisitionClient client;
					const auto response = client.request(
						msap1::AcquisitionCommand::info, 1000);
					require_acquisition_ok(response);
					return json_response(webengine::http::status::ok,
						metadata(response));
				} catch (const std::exception &error) {
					return error_response(
						webengine::http::status::service_unavailable,
						error.what());
				}
			}, webengine::Role::Viewer);

		engine.add_api(webengine::http::verb::get, "/api/v1/adc/samples",
			[sample_reader](const webengine::RequestContext &context) {
				try {
					return json_response(webengine::http::status::ok,
						sample_reader->read(context.request));
				} catch (const std::invalid_argument &error) {
					return error_response(webengine::http::status::bad_request,
						error.what());
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
