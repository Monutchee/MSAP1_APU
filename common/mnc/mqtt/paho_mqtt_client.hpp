#pragma once

#include "mnc/mqtt/mqtt_client.hpp"

#include <memory>

namespace mnc::mqtt {

class PahoMqttClient final : public MqttClient {
public:
	explicit PahoMqttClient(boost::asio::any_io_executor executor);
	~PahoMqttClient() override;

	void configure(const ConnectionOptions &options) override;
	void connect() override;
	void disconnect() noexcept override;
	void publish(const PublishRequest &request) override;
	[[nodiscard]] ConnectionStatus status() const override;

private:
	class Implementation;
	std::unique_ptr<Implementation> implementation_;
};

} // namespace mnc::mqtt
