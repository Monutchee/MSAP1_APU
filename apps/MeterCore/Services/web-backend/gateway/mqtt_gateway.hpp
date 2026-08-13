#pragma once

#include "msap1/mqtt/mqtt_ipc.hpp"
#include "msap1/settings/settings_ipc.hpp"

#include <map>
#include <string>
#include <string_view>

namespace msap1::web {

/** Typed boundary used by HTTP controllers for MQTT runtime and assets. */
class MqttGateway final {
public:
	[[nodiscard]] mqtt::MqttServiceStatus status(int timeout_ms = 3000) const;

	void set_secret(std::string name, std::string value,
		int timeout_ms = 3000) const;
	void clear_secret(std::string name, int timeout_ms = 3000) const;
	[[nodiscard]] bool secret_present(std::string name,
		int timeout_ms = 3000) const;

	void upload_asset(std::string name, std::string contents,
		int timeout_ms = 5000) const;
	void delete_asset(std::string name, int timeout_ms = 3000) const;
	[[nodiscard]] bool asset_present(std::string name,
		int timeout_ms = 3000) const;
	[[nodiscard]] std::string download_asset(std::string name,
		int timeout_ms = 3000) const;

private:
	mutable settings::ipc::SettingsClient settings_;
	mutable mqtt::MqttStatusClient status_;
};

} // namespace msap1::web
