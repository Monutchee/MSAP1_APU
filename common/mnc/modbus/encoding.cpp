#include "mnc/modbus/encoding.hpp"

#include <bit>
#include <stdexcept>

namespace mnc::modbus {
namespace {

std::uint8_t octet(std::byte value)
{
	return std::to_integer<std::uint8_t>(value);
}

} // namespace

std::uint16_t read_u16_be(std::span<const std::byte> bytes)
{
	if (bytes.size() < 2)
		throw std::invalid_argument("truncated 16-bit Modbus value");
	return static_cast<std::uint16_t>((octet(bytes[0]) << 8u) |
		octet(bytes[1]));
}

void append_u16_be(std::vector<std::byte> &bytes, std::uint16_t value)
{
	bytes.push_back(static_cast<std::byte>((value >> 8u) & 0xffu));
	bytes.push_back(static_cast<std::byte>(value & 0xffu));
}

std::vector<std::uint16_t> encode_u16(std::uint16_t value)
{
	return {value};
}

std::vector<std::uint16_t> encode_u32(std::uint32_t value)
{
	return {static_cast<std::uint16_t>(value >> 16u),
		static_cast<std::uint16_t>(value & 0xffffu)};
}

std::vector<std::uint16_t> encode_u64(std::uint64_t value)
{
	return {static_cast<std::uint16_t>(value >> 48u),
		static_cast<std::uint16_t>((value >> 32u) & 0xffffu),
		static_cast<std::uint16_t>((value >> 16u) & 0xffffu),
		static_cast<std::uint16_t>(value & 0xffffu)};
}

std::vector<std::uint16_t> encode_i32(std::int32_t value)
{
	return encode_u32(std::bit_cast<std::uint32_t>(value));
}

std::vector<std::uint16_t> encode_float(float value)
{
	return encode_u32(std::bit_cast<std::uint32_t>(value));
}

} // namespace mnc::modbus
