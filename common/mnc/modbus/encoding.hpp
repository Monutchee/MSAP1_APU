#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace mnc::modbus {

/** Big-endian Modbus register helpers (32/64-bit values use high word first). */
[[nodiscard]] std::uint16_t read_u16_be(std::span<const std::byte> bytes);
void append_u16_be(std::vector<std::byte> &bytes, std::uint16_t value);
[[nodiscard]] std::vector<std::uint16_t> encode_u16(std::uint16_t value);
[[nodiscard]] std::vector<std::uint16_t> encode_u32(std::uint32_t value);
[[nodiscard]] std::vector<std::uint16_t> encode_u64(std::uint64_t value);
[[nodiscard]] std::vector<std::uint16_t> encode_i32(std::int32_t value);
[[nodiscard]] std::vector<std::uint16_t> encode_i64(std::int64_t value);
[[nodiscard]] std::vector<std::uint16_t> encode_float(float value);

} // namespace mnc::modbus
