#include "msap1/mqtt/mqtt_ipc.hpp"

#include <glaze/glaze.hpp>

#include <atomic>
#include <stdexcept>
#include <utility>

namespace msap1::mqtt {
namespace {
constexpr std::uint32_t status_message = 1;
std::uint64_t correlation()
{
	static std::atomic<std::uint64_t> value{1};
	return value.fetch_add(1);
}
} // namespace

std::string connection_state_name(mnc::mqtt::ConnectionState state)
{
	switch (state) {
	case mnc::mqtt::ConnectionState::disconnected: return "disconnected";
	case mnc::mqtt::ConnectionState::connecting: return "connecting";
	case mnc::mqtt::ConnectionState::connected: return "connected";
	case mnc::mqtt::ConnectionState::reconnecting: return "reconnecting";
	}
	return "disconnected";
}

mnc::ipc::Frame encode_status_request()
{
	return {mnc::ipc::FrameKind::request, status_message, correlation(), {}};
}

mnc::ipc::Frame encode_status_response(const MqttServiceStatus &status,
	std::uint64_t correlation_id)
{
	const auto json = glz::write_json(status);
	if (!json)
		throw std::runtime_error("cannot encode MQTT status");
	return {mnc::ipc::FrameKind::response, status_message, correlation_id,
		{reinterpret_cast<const std::byte *>(json->data()),
		 reinterpret_cast<const std::byte *>(json->data() + json->size())}};
}

MqttServiceStatus decode_status_response(const mnc::ipc::Frame &frame)
{
	if (frame.kind != mnc::ipc::FrameKind::response ||
	    frame.message_type != status_message)
		throw std::runtime_error("invalid MQTT status response");
	const std::string_view json{reinterpret_cast<const char *>(frame.payload.data()),
		frame.payload.size()};
	MqttServiceStatus status;
	if (const auto error = glz::read_json(status, json))
		throw std::runtime_error("cannot decode MQTT status");
	return status;
}

MqttStatusClient::MqttStatusClient(std::string path) : path_(std::move(path)) {}

MqttServiceStatus MqttStatusClient::status(int timeout_ms) const
{
	mnc::ipc::BlockingClient client(path_);
	const auto request = encode_status_request();
	const auto response = client.request(request, timeout_ms);
	if (response.correlation_id != request.correlation_id)
		throw std::runtime_error("invalid MQTT status correlation");
	return decode_status_response(response);
}

} // namespace msap1::mqtt
