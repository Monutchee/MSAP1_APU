#pragma once

#include "mnc/modbus/types.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace mnc::modbus {

struct RegisterReadResult {
	ExceptionCode exception = ExceptionCode::none;
	std::vector<std::uint16_t> values;
};

/**
 * Transport-independent Modbus data model.
 *
 * Implementations own product addressing and synchronization. One read()
 * call represents one coherent Modbus request, so snapshot-backed adapters
 * can acquire exactly one source snapshot before projecting all registers.
 */
class RegisterBank {
public:
	virtual ~RegisterBank() = default;
	[[nodiscard]] virtual RegisterReadResult read(
		RegisterTable table, std::uint16_t address,
		std::uint16_t count) const = 0;
	[[nodiscard]] virtual ExceptionCode write_single(
		std::uint16_t address, std::uint16_t value) = 0;
	[[nodiscard]] virtual ExceptionCode write_multiple(
		std::uint16_t address,
		std::span<const std::uint16_t> values) = 0;
};

} // namespace mnc::modbus
