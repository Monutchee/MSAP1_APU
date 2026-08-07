#pragma once

#include "mnc/ipc/ipc.hpp"
#include "mnc/service/service_manager.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace msap1::service_control {

inline constexpr std::string_view socket_path =
	"/run/monutchee/service-manager.sock";
inline constexpr std::uint16_t protocol_version = 1;

enum class Command : std::uint32_t {
	list = 1,
	status = 2,
	start = 3,
	stop = 4,
	restart = 5,
	reload = 6,
};

enum class Status : std::uint16_t {
	ok = 0,
	invalid_request = 1,
	permission_denied = 2,
	operation_failed = 3,
};

struct Request {
	Command command = Command::list;
	std::string service;
};

struct Response {
	Status status = Status::ok;
	std::string message;
	std::vector<mnc::ManagedServiceStatus> services;
};

[[nodiscard]] mnc::ipc::Frame encode_request(const Request &request,
					       std::uint64_t correlation);
[[nodiscard]] Request decode_request(const mnc::ipc::Frame &frame);
[[nodiscard]] mnc::ipc::Frame encode_response(const Response &response,
						std::uint64_t correlation,
						Command command);
[[nodiscard]] Response decode_response(const mnc::ipc::Frame &frame);

class Client {
public:
	explicit Client(std::string path = std::string(socket_path));
	[[nodiscard]] Response request(Command command,
				       std::string service = {}, int timeout_ms = 3000);

private:
	mnc::ipc::BlockingClient client_;
	std::uint64_t next_correlation_ = 1;
};

} // namespace msap1::service_control
