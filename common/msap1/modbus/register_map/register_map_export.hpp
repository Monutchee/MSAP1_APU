#pragma once

#include <cstdint>
#include <string>

namespace msap1::modbus {

enum class RegisterMapFormat : std::uint8_t {
	text,
	csv,
	markdown,
	json,
};

/** Render the compiled register schema; no parallel documentation table exists. */
[[nodiscard]] std::string export_register_map(RegisterMapFormat format);

} // namespace msap1::modbus
