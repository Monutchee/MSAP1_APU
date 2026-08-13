#pragma once

#include "mnc/MeterDataProvider/snapshot/meter_snapshot_provider.hpp"
#include "mnc/modbus/modbus.hpp"
#include "msap1/modbus/register_map/msap1_register_schema.hpp"

#include <cstdint>
#include <span>

namespace msap1::modbus {

using schema::DataType;
using schema::RegisterBlock;
using schema::RegisterDefinition;
using schema::RegisterSource;
using schema::SpecialRegister;
inline constexpr auto register_map_version = schema::register_map_version;

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
		mnc::modbus::FunctionCode function, std::uint16_t address,
		std::uint16_t count) const override;
	[[nodiscard]] mnc::modbus::ExceptionCode write_single(
		std::uint16_t address, std::uint16_t value) override;
	[[nodiscard]] mnc::modbus::ExceptionCode write_multiple(
		std::uint16_t address,
		std::span<const std::uint16_t> values) override;

	/** The same generated map consumed by runtime, tests, and documentation. */
	[[nodiscard]] static constexpr std::span<const RegisterDefinition>
	definitions() noexcept
	{
		return schema::register_map;
	}

	/** Stable reserved blocks, including currently unused future regions. */
	[[nodiscard]] static constexpr std::span<const RegisterBlock>
	blocks() noexcept
	{
		return schema::register_blocks;
	}

private:
	mnc::meter::MeterSnapshotProvider &provider_;
};

} // namespace msap1::modbus
