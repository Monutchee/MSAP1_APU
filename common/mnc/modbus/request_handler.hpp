#pragma once

#include "mnc/modbus/register_bank.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace mnc::modbus {

/** Function-code dispatcher shared by TCP and every RTU port. */
class RequestHandler final {
public:
	explicit RequestHandler(RegisterBank &registers) : registers_(registers) {}

	/**
	 * Handle one PDU. std::nullopt means no response (a different unit or a
	 * valid RTU broadcast). The returned vector is a complete response PDU.
	 */
	[[nodiscard]] std::optional<Response> handle(
		const Request &request, std::uint8_t configured_unit_id);

private:
	[[nodiscard]] std::vector<std::byte> exception(
		std::uint8_t function, ExceptionCode code) const;

	RegisterBank &registers_;
};

} // namespace mnc::modbus
