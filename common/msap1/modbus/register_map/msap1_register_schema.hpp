#pragma once

#include "msap1/modbus/register_map/register_schema.hpp"

#include <array>
#include <optional>

namespace msap1::modbus::schema {

/** External MSAP1 Modbus map contract version. */
inline constexpr std::uint16_t register_map_version = 1;

namespace blocks {

/* Version-1 addresses remain unchanged. Tight legacy groups describe the
 * already published layout; the surrounding blocks reserve stable space for
 * future additions without renumbering any existing register. */
inline constexpr RegisterBlock holding_metadata{
	mnc::modbus::FunctionCode::read_holding_registers,
	0x0000, 0x0100, "holding.metadata"};
inline constexpr RegisterBlock basic_frequency{
	mnc::modbus::FunctionCode::read_input_registers,
	0x0000, 0x0002, "basic.frequency"};
inline constexpr RegisterBlock basic_voltage_ln{
	mnc::modbus::FunctionCode::read_input_registers,
	0x0002, 0x0006, "basic.voltage_ln"};
inline constexpr RegisterBlock basic_current{
	mnc::modbus::FunctionCode::read_input_registers,
	0x0008, 0x0008, "basic.current"};
inline constexpr RegisterBlock basic_status{
	mnc::modbus::FunctionCode::read_input_registers,
	0x0010, 0x0010, "basic.status"};
inline constexpr RegisterBlock basic_legacy_reserve{
	mnc::modbus::FunctionCode::read_input_registers,
	0x0020, 0x00e0, "basic.legacy_reserve"};
inline constexpr RegisterBlock basic_extension{
	mnc::modbus::FunctionCode::read_input_registers,
	0x0100, 0x0f00, "basic.extension"};
inline constexpr RegisterBlock cycles_150_180{
	mnc::modbus::FunctionCode::read_input_registers,
	0x1000, 0x1000, "cycles_150_180"};
inline constexpr RegisterBlock minute_10{
	mnc::modbus::FunctionCode::read_input_registers,
	0x2000, 0x1000, "minute_10"};
inline constexpr RegisterBlock hour_2{
	mnc::modbus::FunctionCode::read_input_registers,
	0x3000, 0x1000, "hour_2"};
inline constexpr RegisterBlock voltage_harmonics{
	mnc::modbus::FunctionCode::read_input_registers,
	0x4000, 0x1000, "voltage_harmonics"};
inline constexpr RegisterBlock current_harmonics{
	mnc::modbus::FunctionCode::read_input_registers,
	0x5000, 0x1000, "current_harmonics"};
inline constexpr RegisterBlock power_quality{
	mnc::modbus::FunctionCode::read_input_registers,
	0x6000, 0x1000, "power_quality"};

} // namespace blocks

inline constexpr std::array register_blocks{
	blocks::holding_metadata,
	blocks::basic_frequency,
	blocks::basic_voltage_ln,
	blocks::basic_current,
	blocks::basic_status,
	blocks::basic_legacy_reserve,
	blocks::basic_extension,
	blocks::cycles_150_180,
	blocks::minute_10,
	blocks::hour_2,
	blocks::voltage_harmonics,
	blocks::current_harmonics,
	blocks::power_quality,
};

using Id = mnc::meter::MeterAttributeId;
using Key = mnc::meter::MeterAttributeKey;
using Period = mnc::meter::MeasurementPeriod;

inline constexpr auto metadata = make_special_block(
	blocks::holding_metadata,
	std::array{
		SpecialDefinition{SpecialRegister::map_version, DataType::uint16,
			std::nullopt},
		SpecialDefinition{SpecialRegister::word_order_marker,
			DataType::uint16, std::nullopt},
		SpecialDefinition{SpecialRegister::attribute_count,
			DataType::uint16, std::nullopt},
	});

inline constexpr auto basic_frequency = make_attribute_block(
	blocks::basic_frequency, Period::Basic, DataType::float32,
	std::array{Key{Id::Frequency, std::nullopt}});

inline constexpr auto basic_voltage_ln = make_attribute_block(
	blocks::basic_voltage_ln, Period::Basic, DataType::float32,
	std::array{
		Key{Id::VanRms, std::nullopt},
		Key{Id::VbnRms, std::nullopt},
		Key{Id::VcnRms, std::nullopt},
	});

inline constexpr auto basic_current = make_attribute_block(
	blocks::basic_current, Period::Basic, DataType::float32,
	std::array{
		Key{Id::IaRms, std::nullopt},
		Key{Id::IbRms, std::nullopt},
		Key{Id::IcRms, std::nullopt},
		Key{Id::InRms, std::nullopt},
	});

inline constexpr auto basic_status = make_special_block(
	blocks::basic_status,
	std::array{
		SpecialDefinition{SpecialRegister::quality_mask, DataType::uint16,
			Period::Basic},
		SpecialDefinition{SpecialRegister::period, DataType::uint16,
			Period::Basic},
		SpecialDefinition{SpecialRegister::source_sequence, DataType::uint32,
			Period::Basic},
		SpecialDefinition{SpecialRegister::configuration_generation,
			DataType::uint32, Period::Basic},
	});

/** One sorted source of truth used by runtime, tests, and map documentation. */
inline constexpr auto register_map = concat(
	metadata, basic_frequency, basic_voltage_ln, basic_current, basic_status);

inline constexpr std::array published_measurement_attributes{
	Key{Id::Frequency, std::nullopt},
	Key{Id::VanRms, std::nullopt},
	Key{Id::VbnRms, std::nullopt},
	Key{Id::VcnRms, std::nullopt},
	Key{Id::IaRms, std::nullopt},
	Key{Id::IbRms, std::nullopt},
	Key{Id::IcRms, std::nullopt},
	Key{Id::InRms, std::nullopt},
};

static_assert(validate_register_map(register_blocks, register_map),
	"MSAP1 Modbus blocks or generated register definitions are invalid");

} // namespace msap1::modbus::schema
