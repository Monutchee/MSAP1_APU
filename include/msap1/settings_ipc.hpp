#pragma once

#include "mnc/ipc/ipc.hpp"
#include "msap1/settings.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace msap1::settings::ipc {

inline constexpr std::uint32_t protocol_version = 1;

enum class Command : std::uint32_t {
	get_active = 1,
	get_draft = 2,
	patch_draft = 3,
	get_diff = 4,
	commit_draft = 5,
	discard_draft = 6,
	list_revisions = 7,
	get_revision = 8,
	restore_to_draft = 9,
	factory_reset = 10,
	get_transaction_status = 11,
	set_secret = 12,
	get_secret_status = 13,
	subscribe_events = 14,
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
	std::uint64_t expected_revision = 0;
	std::uint64_t expected_generation = 0;
	std::uint64_t revision = 0;
	bool confirmed = false;
	std::string message;
	std::string json;
};

struct Response {
	Status status = Status::ok;
	std::uint64_t revision = 0;
	std::uint64_t generation = 0;
	std::string transaction_id;
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

private:
	std::string path_;
};

} // namespace msap1::settings::ipc
