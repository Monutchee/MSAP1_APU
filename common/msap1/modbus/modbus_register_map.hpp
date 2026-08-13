#pragma once

#include "mnc/MeterDataProvider/snapshot/meter_snapshot_provider.hpp"
#include "mnc/modbus/modbus.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace msap1::modbus {

/** External MSAP1 Modbus map contract version. */
inline constexpr std::uint16_t register_map_version = 1;

enum class MeterField : std::uint8_t {
	frequency,
	voltage_ln_a,
	voltage_ln_b,
	voltage_ln_c,
	current_a,
	current_b,
	current_c,
	current_neutral,
	quality_mask,
	period,
	source_sequence,
	configuration_generation,
	map_version,
	word_order_marker,
	attribute_count,
};

enum class DataType : std::uint8_t { uint16, uint32, float32 };

struct RegisterDefinition {
	mnc::modbus::RegisterTable table;
	std::uint16_t address;
	std::uint16_t words;
	DataType type;
	MeterField field;
};

/**
 * Product register map backed by one coherent Basic meter snapshot per read.
 *
 * All Modbus bytes are big-endian. 32-bit values and IEEE-754 float32 values
 * use high-word-first order. Invalid or unavailable electrical readings are
 * encoded as quiet NaN and identified by a cleared bit in quality_mask.
 */
class Msap1RegisterBank final : public mnc::modbus::RegisterBank {
public:
	explicit Msap1RegisterBank(mnc::meter::MeterSnapshotProvider &provider)
		: provider_(provider)
	{
	}

	[[nodiscard]] mnc::modbus::RegisterReadResult read(
		mnc::modbus::RegisterTable table, std::uint16_t address,
		std::uint16_t count) const override;
	[[nodiscard]] mnc::modbus::ExceptionCode write_single(
		std::uint16_t address, std::uint16_t value) override;
	[[nodiscard]] mnc::modbus::ExceptionCode write_multiple(
		std::uint16_t address,
		std::span<const std::uint16_t> values) override;

	[[nodiscard]] static std::span<const RegisterDefinition> definitions();

private:
	mnc::meter::MeterSnapshotProvider &provider_;
};

} // namespace msap1::modbus
