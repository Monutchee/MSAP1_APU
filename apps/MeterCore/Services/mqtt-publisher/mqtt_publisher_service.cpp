#include "mqtt_publisher_service.hpp"

#include "msap1/settings/settings.hpp"
#include "msap1/settings/settings_ipc.hpp"
#include "mnc/settings/settings.hpp"

#include <glaze/glaze.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <utility>

#include <sys/stat.h>

namespace msap1::mqtt::daemon {
namespace {
using namespace std::chrono_literals;

mnc::mqtt::Transport transport(msap1::settings::MqttTransport value)
{
	switch (value) {
	case msap1::settings::MqttTransport::mqtt:
		return mnc::mqtt::Transport::mqtt;
	case msap1::settings::MqttTransport::mqtts:
		return mnc::mqtt::Transport::mqtts;
	case msap1::settings::MqttTransport::ws:
		return mnc::mqtt::Transport::ws;
	case msap1::settings::MqttTransport::wss:
		return mnc::mqtt::Transport::wss;
	}
	return mnc::mqtt::Transport::mqtt;
}

} // namespace

MqttPublisherService::MqttPublisherService()
	: Service("MSAP1 MQTT publisher", "mqtt-publisher"),
	  client_(context_.get_executor()),
	  scheduler_(context_.get_executor(), provider_, client_),
	  status_server_(context_.get_executor(), std::string(socket_path))
{
}

void MqttPublisherService::on_start()
{
	const auto settings = msap1::settings::ipc::SettingsClient{}.active();
	settings.mqtt.validate();
	{
		std::scoped_lock lock(settings_mutex_);
		observed_settings_ = settings.mqtt;
	}
	apply(settings.mqtt);
	status_server_.start(
		[this](auto connection, auto frame) {
			handle_status(std::move(connection), std::move(frame));
		},
		[this](const std::string &error) {
			(void)logger().write(mnc::logging::Priority::warning,
				"MQTT status IPC error: " + error, "ipc_error");
		});
	if (::chmod(socket_path.data(), 0660) != 0)
		throw std::runtime_error("cannot set MQTT status socket mode");
	io_worker_ = std::thread([this] {
		try {
			context_.run();
		} catch (const std::exception &error) {
			failed_ = true;
			(void)logger().write(mnc::logging::Priority::critical,
				"MQTT I/O worker failed: " + std::string(error.what()),
				"io_worker_failed");
			request_stop();
		}
	});
	settings_watcher_ = std::thread([this] { watch_settings(); });
}

void MqttPublisherService::apply(
	const msap1::settings::MqttSettings &settings)
{
	std::scoped_lock runtime_lock(runtime_mutex_);
	scheduler_.stop();
	client_.disconnect();
	{
		std::scoped_lock lock(settings_mutex_);
		active_settings_ = settings;
	}
	if (!settings.enabled) {
		(void)logger().write(mnc::logging::Priority::notice,
			"MQTT publishing is disabled", "publisher_disabled");
		return;
	}

	const auto secrets =
		msap1::settings::ipc::SettingsClient{}.runtime_mqtt_credentials();
	const auto assets = msap1::settings::ipc::SettingsClient{}.
		runtime_mqtt_assets();
	{
		std::scoped_lock lock(settings_mutex_);
		observed_secrets_ = secrets;
		observed_assets_ = assets;
	}
	materialize_tls_assets(assets);
	mnc::mqtt::ConnectionOptions connection{
		.transport = transport(settings.connection.transport),
		.host = settings.connection.broker_host,
		.port = settings.connection.broker_port,
		.websocket_path = settings.connection.websocket_path,
		.client_id = settings.connection.client_id,
		.username = settings.connection.username,
		.password = secrets.contains("password") ? secrets.at("password") : "",
		.keep_alive = std::chrono::seconds(settings.connection.keep_alive_seconds),
		.connect_timeout = std::chrono::seconds(
			settings.connection.connect_timeout_seconds),
		.reconnect_min = std::chrono::seconds(
			settings.connection.reconnect_min_seconds),
		.reconnect_max = std::chrono::seconds(
			settings.connection.reconnect_max_seconds),
		.tls = {
			.enabled = settings.connection.transport ==
					msap1::settings::MqttTransport::mqtts ||
				settings.connection.transport ==
					msap1::settings::MqttTransport::wss,
			.use_system_ca = settings.tls.use_system_ca,
			.verify_peer = settings.tls.verify_peer,
			.verify_hostname = settings.tls.verify_hostname,
			.ca_file = settings.tls.use_system_ca
				? "/etc/ssl/certs/ca-certificates.crt"
				: (runtime_tls_root_ / "ca.pem").string(),
			.client_certificate_file = settings.tls.use_client_certificate
				? (runtime_tls_root_ / "client-certificate.pem").string() : "",
			.client_private_key_file = settings.tls.use_client_certificate
				? (runtime_tls_root_ / "client-key.pem").string() : "",
			.client_private_key_password =
				secrets.contains("private_key_passphrase")
				? secrets.at("private_key_passphrase") : "",
		}};
	client_.configure(connection);
	scheduler_.configure(settings.publications);
	scheduler_.start();
	reconnect_delay_ = connection.reconnect_min;
	next_connect_attempt_ = std::chrono::steady_clock::now();
}

void MqttPublisherService::materialize_tls_assets(
	const std::map<std::string, std::string> &assets)
{
	std::filesystem::create_directories(runtime_tls_root_);
	std::filesystem::permissions(runtime_tls_root_,
		std::filesystem::perms::owner_all,
		std::filesystem::perm_options::replace);
	const auto write = [&](std::string_view key, std::string_view filename) {
		const auto found = assets.find(std::string(key));
		const auto path = runtime_tls_root_ / filename;
		if (found != assets.end())
			mnc::settings::AtomicFileWriter::write(path, found->second, 0600);
		else {
			std::error_code ignored;
			std::filesystem::remove(path, ignored);
		}
	};
	write("ca", "ca.pem");
	write("client-certificate", "client-certificate.pem");
	write("client-key", "client-key.pem");
}

void MqttPublisherService::try_connect()
{
	std::scoped_lock runtime_lock(runtime_mutex_);
	bool enabled = false;
	msap1::settings::MqttSettings settings;
	{
		std::scoped_lock lock(settings_mutex_);
		enabled = active_settings_.enabled;
		settings = active_settings_;
	}
	const auto state = client_.status().state;
	if (!enabled || state == mnc::mqtt::ConnectionState::connected ||
	    state == mnc::mqtt::ConnectionState::connecting || stopping_) {
		if (enabled && !stopping_)
			scheduler_.flush_pending();
		return;
	}
	const auto now = std::chrono::steady_clock::now();
	if (now < next_connect_attempt_)
		return;
	try {
		client_.connect();
		reconnect_delay_ = std::chrono::seconds(
			settings.connection.reconnect_min_seconds);
		next_connect_attempt_ = now;
		scheduler_.flush_pending();
		(void)logger().write(mnc::logging::Priority::notice,
			"MQTT broker connected", "broker_connected");
	} catch (const std::exception &error) {
		const auto maximum = std::chrono::seconds(
			settings.connection.reconnect_max_seconds);
		next_connect_attempt_ = now + reconnect_delay_;
		reconnect_delay_ = std::min(reconnect_delay_ * 2, maximum);
		(void)logger().write(mnc::logging::Priority::warning,
			"MQTT broker connection deferred: " +
				std::string(error.what()) + "; retrying in " +
				std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
					next_connect_attempt_ - now).count()) + " seconds",
			"broker_reconnecting");
	}
}

void MqttPublisherService::stop_runtime() noexcept
{
	std::scoped_lock runtime_lock(runtime_mutex_);
	scheduler_.stop();
	client_.disconnect();
}

void MqttPublisherService::watch_settings()
{
	auto next_settings_check = std::chrono::steady_clock::now();
	while (!stopping_) {
		std::this_thread::sleep_for(200ms);
		if (stopping_)
			break;

		const auto now = std::chrono::steady_clock::now();
		if (now < next_settings_check) {
			try_connect();
			continue;
		}
		next_settings_check = now + 2s;

		try {
			const msap1::settings::ipc::SettingsClient settings_client;
			const auto settings = settings_client.active(1500);
			const auto secrets = settings_client.runtime_mqtt_credentials(1500);
			const auto assets = settings_client.runtime_mqtt_assets(1500);
			bool changed = false;
			{
				std::scoped_lock lock(settings_mutex_);
				changed = settings.mqtt != observed_settings_ ||
					secrets != observed_secrets_ || assets != observed_assets_;
				if (changed) {
					observed_settings_ = settings.mqtt;
					observed_secrets_ = secrets;
					observed_assets_ = assets;
				}
			}
			if (changed)
				request_reload();
			else
				try_connect();
		} catch (const std::exception &error) {
			(void)logger().write(mnc::logging::Priority::debug,
				"MQTT settings check deferred: " +
					std::string(error.what()),
				"settings_check_deferred");
		}
	}
}

void MqttPublisherService::on_reload()
{
	const auto settings = msap1::settings::ipc::SettingsClient{}.active();
	settings.mqtt.validate();
	{
		std::scoped_lock lock(settings_mutex_);
		observed_settings_ = settings.mqtt;
	}
	apply(settings.mqtt);
	(void)logger().write(mnc::logging::Priority::notice,
		"MQTT configuration applied", "configuration_applied");
}

void MqttPublisherService::on_stop() noexcept
{
	stopping_ = true;
	if (settings_watcher_.joinable())
		settings_watcher_.join();
	stop_runtime();
	status_server_.stop();
	context_.stop();
	if (io_worker_.joinable())
		io_worker_.join();
}

MqttServiceStatus MqttPublisherService::status_snapshot() const
{
	const auto client_status = client_.status();
	std::scoped_lock lock(settings_mutex_);
	return {.enabled = active_settings_.enabled,
		.state = active_settings_.enabled
			? connection_state_name(client_status.state) : "disabled",
		.server_uri = client_status.server_uri,
		.last_error = client_status.last_error,
		.successful_publishes = client_status.successful_publishes,
		.last_successful_publish_unix_ms =
			client_status.last_successful_publish_unix_ms,
		.publications = scheduler_.statistics()};
}

void MqttPublisherService::handle_status(
	mnc::ipc::UnixStreamServer::Connection connection, mnc::ipc::Frame frame)
{
	if (frame.kind != mnc::ipc::FrameKind::request || frame.message_type != 1)
		return;
	connection->post_send(encode_status_response(
		status_snapshot(), frame.correlation_id));
}

mnc::ServiceHealth MqttPublisherService::health() const
{
	if (failed_)
		return {false, "MQTT I/O worker failed"};
	const auto status = status_snapshot();
	if (!status.enabled)
		return {true, "MQTT publishing disabled"};
	if (status.state != "connected")
		return {true, "MQTT broker reconnecting: " + status.last_error};
	return {true, "MQTT publisher connected"};
}

} // namespace msap1::mqtt::daemon
