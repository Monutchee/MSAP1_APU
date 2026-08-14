#pragma once

#include "mnc/ipc/ipc.hpp"
#include "mnc/mqtt/mqtt_client.hpp"
#include "msap1/mqtt/meter_publication_scheduler.hpp"

#include <map>
#include <string>

namespace msap1::mqtt {

inline constexpr std::string_view socket_path =
	"/run/monutchee/mqtt-publisher.sock";

struct MqttServiceStatus {
	bool enabled = false;
	std::string state = "disabled";
	std::string server_uri;
	std::string last_error;
	std::uint64_t successful_publishes = 0;
	std::int64_t last_successful_publish_unix_ms = 0;
	std::map<std::string, PublicationStatistics> publications;
};

[[nodiscard]] std::string connection_state_name(
	mnc::mqtt::ConnectionState state);
[[nodiscard]] mnc::ipc::Frame encode_status_request();
[[nodiscard]] mnc::ipc::Frame encode_status_response(
	const MqttServiceStatus &status, std::uint64_t correlation);
[[nodiscard]] MqttServiceStatus decode_status_response(
	const mnc::ipc::Frame &frame);

class MqttStatusClient final {
public:
	explicit MqttStatusClient(std::string path = std::string(socket_path));
	[[nodiscard]] MqttServiceStatus status(int timeout_ms = 3000) const;

private:
	std::string path_;
};

} // namespace msap1::mqtt
