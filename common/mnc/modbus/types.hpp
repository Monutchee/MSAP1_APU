#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

namespace mnc::modbus {

enum class FunctionCode : std::uint8_t {
	read_holding_registers = 0x03,
	read_input_registers = 0x04,
	write_single_register = 0x06,
	write_multiple_registers = 0x10,
};

enum class ExceptionCode : std::uint8_t {
	none = 0x00,
	illegal_function = 0x01,
	illegal_data_address = 0x02,
	illegal_data_value = 0x03,
	server_device_failure = 0x04,
	/** Request accepted; the server needs more time to finish it. */
	acknowledge = 0x05,
	/** Server cannot accept the request while a long operation is active. */
	server_device_busy = 0x06,
	/** Extended-memory parity or consistency check failed. */
	memory_parity_error = 0x08,
	/** Gateway could not establish a path to the target device. */
	gateway_path_unavailable = 0x0a,
	/** Gateway target did not return a response. */
	gateway_target_device_failed_to_respond = 0x0b,
};

/** Return whether a function code selects a Modbus register read table. */
constexpr bool is_register_read(FunctionCode function) noexcept
{
	return function == FunctionCode::read_holding_registers ||
	       function == FunctionCode::read_input_registers;
}

/** Transport-neutral request after TCP/RTU framing has been removed. */
struct Request {
	std::uint8_t unit_id = 1;
	std::span<const std::byte> pdu;
	bool broadcast_allowed = false;
};

/** Transport-neutral response PDU. A missing response is intentional. */
struct Response {
	std::uint8_t unit_id = 1;
	std::vector<std::byte> pdu;
};

using ErrorHandler = std::function<void(std::string_view transport,
	std::string_view message)>;

} // namespace mnc::modbus
