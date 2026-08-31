#pragma once

#include "mnc/ipc/ipc.hpp"
#include "msap1/settings/settings.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <map>

namespace msap1::settings::ipc {

inline constexpr std::uint32_t protocol_version = 4;

enum class Command : std::uint32_t {
	get_active = 1,
	save_active = 2,
	factory_reset = 3,
	set_secret = 4,
	get_secret_status = 5,
	subscribe_events = 6,
	set_named_secret = 7,
	clear_named_secret = 8,
	get_named_secret_status = 9,
	resolve_mqtt_credentials = 10,
	put_asset = 11,
	delete_asset = 12,
	get_asset_status = 13,
	download_asset = 14,
	resolve_mqtt_assets = 15,
	resolve_data_channel_credentials = 16,
	resolve_data_channel_assets = 17,
};

enum class Status : std::uint32_t {
	ok = 0,
	invalid_request = 1,
	conflict = 2,
	permission_denied = 3,
	apply_failed = 4,
	recovery_mode = 5,
	internal_error = 6,
};

struct Request {
	Command command = Command::get_active;
	bool confirmed = false;
	std::string name;
	std::string json;
};

struct Response {
	Status status = Status::ok;
	std::string content_hash;
	std::string message;
	std::string json;
};

[[nodiscard]] mnc::ipc::Frame encode_request(const Request &request);
[[nodiscard]] Request decode_request(const mnc::ipc::Frame &frame);
[[nodiscard]] mnc::ipc::Frame encode_response(
	const Response &response, std::uint64_t correlation_id,
	Command command);
/** Encode a server-pushed settings event using the response payload schema. */
[[nodiscard]] mnc::ipc::Frame encode_event(
	const Response &event, Command command = Command::subscribe_events);
[[nodiscard]] Response decode_response(const mnc::ipc::Frame &frame);

class SettingsClient final {
public:
	explicit SettingsClient(std::string path = std::string(socket_path));
	[[nodiscard]] Response request(Request request, int timeout_ms = 5000) const;
	[[nodiscard]] ProductSettings active(int timeout_ms = 3000) const;
	void set_secret(std::string name, std::string value,
		int timeout_ms = 5000) const;
	void clear_secret(std::string name, int timeout_ms = 5000) const;
	[[nodiscard]] bool secret_present(std::string name,
		int timeout_ms = 3000) const;
	[[nodiscard]] std::map<std::string, std::string>
	runtime_mqtt_credentials(int timeout_ms = 3000) const;
	[[nodiscard]] std::map<std::string, std::string>
	runtime_mqtt_assets(int timeout_ms = 3000) const;
	[[nodiscard]] std::map<std::string, std::string>
	runtime_data_channel_credentials(std::string channel_id,
		int timeout_ms = 3000) const;
	[[nodiscard]] std::map<std::string, std::string>
	runtime_data_channel_assets(std::string channel_id,
		int timeout_ms = 3000) const;
	void put_asset(std::string name, std::string contents,
		int timeout_ms = 5000) const;
	void delete_asset(std::string name, int timeout_ms = 5000) const;
	[[nodiscard]] bool asset_present(std::string name,
		int timeout_ms = 3000) const;
	[[nodiscard]] std::string download_asset(std::string name,
		int timeout_ms = 5000) const;

private:
	std::string path_;
};

} // namespace msap1::settings::ipc
