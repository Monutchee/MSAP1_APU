#include "mnc/mqtt/paho_mqtt_client.hpp"

#include <boost/asio/post.hpp>
#include <mqtt/async_client.h>

#include <chrono>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace mnc::mqtt {

class PahoMqttClient::Implementation {
public:
	explicit Implementation(boost::asio::any_io_executor executor)
		: executor_(std::move(executor))
	{
	}

	void configure(const ConnectionOptions &configuration)
	{
		disconnect();
		const auto uri = server_uri(configuration);
		std::uint64_t generation = 0;
		{
			std::scoped_lock lock(mutex_);
			generation = ++generation_;
		}
		auto client = std::make_unique<::mqtt::async_client>(
			uri, configuration.client_id);
		client->set_connected_handler([this, generation](const std::string &) {
			boost::asio::post(executor_, [this, generation] {
				std::scoped_lock lock(mutex_);
				if (generation != generation_)
					return;
				status_.state = ConnectionState::connected;
				status_.last_error.clear();
			});
		});
		client->set_connection_lost_handler([this, generation](const std::string &cause) {
			boost::asio::post(executor_, [this, generation, cause] {
				std::scoped_lock lock(mutex_);
				if (generation != generation_)
					return;
				status_.state = ConnectionState::reconnecting;
				status_.last_error = cause;
			});
		});

		::mqtt::connect_options options;
		options.set_mqtt_version(MQTTVERSION_3_1_1);
		options.set_clean_session(true);
		options.set_keep_alive_interval(configuration.keep_alive);
		options.set_connect_timeout(configuration.connect_timeout);
		// Reconnect scheduling belongs to the service.  Keeping it out of
		// Paho gives the product one observable exponential-backoff policy and
		// avoids overlapping an automatic reconnect with an explicit reload.
		if (!configuration.username.empty())
			options.set_user_name(configuration.username);
		if (!configuration.password.empty())
			options.set_password(configuration.password);

		if (configuration.tls.enabled) {
			::mqtt::ssl_options ssl;
			if (!configuration.tls.ca_file.empty())
				ssl.set_trust_store(configuration.tls.ca_file);
			if (!configuration.tls.client_certificate_file.empty())
				ssl.set_key_store(configuration.tls.client_certificate_file);
			if (!configuration.tls.client_private_key_file.empty())
				ssl.set_private_key(configuration.tls.client_private_key_file);
			if (!configuration.tls.client_private_key_password.empty())
				ssl.set_private_key_password(
					configuration.tls.client_private_key_password);
			ssl.set_enable_server_cert_auth(configuration.tls.verify_peer);
			ssl.set_verify(configuration.tls.verify_hostname);
			options.set_ssl(std::move(ssl));
		}

		std::scoped_lock lock(mutex_);
		configuration_ = configuration;
		connect_options_ = std::move(options);
		client_ = std::move(client);
		status_ = {.state = ConnectionState::disconnected,
			.server_uri = uri};
	}

	void connect()
	{
		::mqtt::async_client *client = nullptr;
		::mqtt::connect_options options;
		std::chrono::seconds timeout;
		{
			std::scoped_lock lock(mutex_);
			if (!client_)
				throw std::logic_error("MQTT client is not configured");
			status_.state = ConnectionState::connecting;
			client = client_.get();
			options = connect_options_;
			timeout = configuration_.connect_timeout;
		}
		try {
			if (!client->connect(std::move(options))->wait_for(timeout))
				throw std::runtime_error("MQTT connect timed out");
			std::scoped_lock lock(mutex_);
			status_.state = ConnectionState::connected;
			status_.last_error.clear();
		} catch (const std::exception &error) {
			std::scoped_lock lock(mutex_);
			status_.state = ConnectionState::reconnecting;
			status_.last_error = error.what();
			throw;
		}
	}

	void disconnect() noexcept
	{
		std::unique_ptr<::mqtt::async_client> old;
		{
			std::scoped_lock lock(mutex_);
			++generation_;
			old = std::move(client_);
			status_.state = ConnectionState::disconnected;
		}
		if (old) {
			try {
				if (old->is_connected())
					old->disconnect()->wait();
			} catch (...) {
			}
		}
	}

	void publish(const PublishRequest &request)
	{
		::mqtt::async_client *client = nullptr;
		std::chrono::seconds timeout;
		{
			std::scoped_lock lock(mutex_);
			if (!client_ || status_.state != ConnectionState::connected)
				throw std::runtime_error("MQTT broker is not connected");
			client = client_.get();
			timeout = configuration_.connect_timeout;
		}
		try {
			if (!client->publish(request.topic, request.payload.data(),
				request.payload.size(), request.qos, request.retain)
					->wait_for(timeout))
				throw std::runtime_error("MQTT publish timed out");
			std::scoped_lock lock(mutex_);
			++status_.successful_publishes;
			status_.last_successful_publish_unix_ms =
				std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::system_clock::now().time_since_epoch()).count();
			status_.last_error.clear();
		} catch (const std::exception &error) {
			std::scoped_lock lock(mutex_);
			status_.state = ConnectionState::reconnecting;
			status_.last_error = error.what();
			throw;
		}
	}

	ConnectionStatus status() const
	{
		std::scoped_lock lock(mutex_);
		return status_;
	}

private:
	boost::asio::any_io_executor executor_;
	mutable std::mutex mutex_;
	ConnectionOptions configuration_;
	::mqtt::connect_options connect_options_;
	std::unique_ptr<::mqtt::async_client> client_;
	ConnectionStatus status_;
	std::uint64_t generation_{0};
};

PahoMqttClient::PahoMqttClient(boost::asio::any_io_executor executor)
	: implementation_(std::make_unique<Implementation>(std::move(executor)))
{
}

PahoMqttClient::~PahoMqttClient() = default;
void PahoMqttClient::configure(const ConnectionOptions &options)
{
	implementation_->configure(options);
}
void PahoMqttClient::connect() { implementation_->connect(); }
void PahoMqttClient::disconnect() noexcept { implementation_->disconnect(); }
void PahoMqttClient::publish(const PublishRequest &request)
{
	implementation_->publish(request);
}
ConnectionStatus PahoMqttClient::status() const
{
	return implementation_->status();
}

} // namespace mnc::mqtt
