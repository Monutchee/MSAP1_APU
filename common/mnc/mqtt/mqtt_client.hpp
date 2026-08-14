#pragma once

#include <boost/asio/any_io_executor.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace mnc::mqtt {

enum class Transport : std::uint8_t { mqtt, mqtts, ws, wss };

struct TlsOptions {
	bool enabled = false;
	bool use_system_ca = true;
	bool verify_peer = true;
	bool verify_hostname = true;
	std::string ca_file;
	std::string client_certificate_file;
	std::string client_private_key_file;
	std::string client_private_key_password;
};

struct ConnectionOptions {
	Transport transport = Transport::mqtt;
	std::string host;
	std::uint16_t port = 1883;
	std::string websocket_path = "/mqtt";
	std::string client_id;
	std::string username;
	std::string password;
	std::chrono::seconds keep_alive{30};
	std::chrono::seconds connect_timeout{10};
	std::chrono::seconds reconnect_min{1};
	std::chrono::seconds reconnect_max{60};
	TlsOptions tls;
};

struct PublishRequest {
	std::string publication_id;
	std::string topic;
	std::string payload;
	std::uint8_t qos = 1;
	bool retain = false;
};

enum class ConnectionState : std::uint8_t {
	disconnected,
	connecting,
	connected,
	reconnecting,
};

struct ConnectionStatus {
	ConnectionState state = ConnectionState::disconnected;
	std::string server_uri;
	std::string last_error;
	std::uint64_t successful_publishes = 0;
	std::int64_t last_successful_publish_unix_ms = 0;
};

/** Broker-independent interface used by the scheduler and its unit tests. */
class MqttClient {
public:
	virtual ~MqttClient() = default;
	virtual void configure(const ConnectionOptions &options) = 0;
	virtual void connect() = 0;
	virtual void disconnect() noexcept = 0;
	virtual void publish(const PublishRequest &request) = 0;
	[[nodiscard]] virtual ConnectionStatus status() const = 0;
};

/** Build the Paho server URI, including the WebSocket path when required. */
[[nodiscard]] std::string server_uri(const ConnectionOptions &options);

} // namespace mnc::mqtt
