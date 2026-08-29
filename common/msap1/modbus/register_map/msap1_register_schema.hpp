#pragma once

#include "msap1/modbus/register_map/register_schema.hpp"

#include <array>
#include <optional>

namespace msap1::modbus::schema {

/** External MSAP1 Modbus map contract version. */
inline constexpr std::uint16_t register_map_version = 2;

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

/* M17 uses explicit published addresses inside the already-reserved Basic
 * and Min10 regions. Each scalar is one high-word-first 64-bit value. */
inline constexpr RegisterBlock energy_active_import_layout{
	blocks::basic_extension.function, 0x0100, 0x0010,
	"basic.energy.active_import"};
inline constexpr RegisterBlock energy_active_export_layout{
	blocks::basic_extension.function, 0x0110, 0x0010,
	"basic.energy.active_export"};
inline constexpr RegisterBlock energy_apparent_layout{
	blocks::basic_extension.function, 0x0120, 0x0010,
	"basic.energy.apparent"};
inline constexpr RegisterBlock energy_quadrant_i_layout{
	blocks::basic_extension.function, 0x0130, 0x0010,
	"basic.energy.quadrant_i"};
inline constexpr RegisterBlock energy_quadrant_ii_layout{
	blocks::basic_extension.function, 0x0140, 0x0010,
	"basic.energy.quadrant_ii"};
inline constexpr RegisterBlock energy_quadrant_iii_layout{
	blocks::basic_extension.function, 0x0150, 0x0010,
	"basic.energy.quadrant_iii"};
inline constexpr RegisterBlock energy_quadrant_iv_layout{
	blocks::basic_extension.function, 0x0160, 0x0010,
	"basic.energy.quadrant_iv"};
inline constexpr RegisterBlock energy_metadata_layout{
	blocks::basic_extension.function, 0x0170, 0x0020,
	"basic.energy.metadata"};

inline constexpr auto active_import_energy = make_attribute_block(
	energy_active_import_layout, Period::Basic, DataType::uint64,
	std::array{Key{Id::ActiveImportEnergyA, std::nullopt},
		Key{Id::ActiveImportEnergyB, std::nullopt},
		Key{Id::ActiveImportEnergyC, std::nullopt},
		Key{Id::ActiveImportEnergyTotal, std::nullopt}});
inline constexpr auto active_export_energy = make_attribute_block(
	energy_active_export_layout, Period::Basic, DataType::uint64,
	std::array{Key{Id::ActiveExportEnergyA, std::nullopt},
		Key{Id::ActiveExportEnergyB, std::nullopt},
		Key{Id::ActiveExportEnergyC, std::nullopt},
		Key{Id::ActiveExportEnergyTotal, std::nullopt}});
inline constexpr auto apparent_energy = make_attribute_block(
	energy_apparent_layout, Period::Basic, DataType::uint64,
	std::array{Key{Id::ApparentEnergyA, std::nullopt},
		Key{Id::ApparentEnergyB, std::nullopt},
		Key{Id::ApparentEnergyC, std::nullopt},
		Key{Id::ApparentEnergyTotal, std::nullopt}});
inline constexpr auto reactive_energy_quadrant_i = make_attribute_block(
	energy_quadrant_i_layout, Period::Basic, DataType::uint64,
	std::array{Key{Id::ReactiveEnergyQuadrantIA, std::nullopt},
		Key{Id::ReactiveEnergyQuadrantIB, std::nullopt},
		Key{Id::ReactiveEnergyQuadrantIC, std::nullopt},
		Key{Id::ReactiveEnergyQuadrantITotal, std::nullopt}});
inline constexpr auto reactive_energy_quadrant_ii = make_attribute_block(
	energy_quadrant_ii_layout, Period::Basic, DataType::uint64,
	std::array{Key{Id::ReactiveEnergyQuadrantIIA, std::nullopt},
		Key{Id::ReactiveEnergyQuadrantIIB, std::nullopt},
		Key{Id::ReactiveEnergyQuadrantIIC, std::nullopt},
		Key{Id::ReactiveEnergyQuadrantIITotal, std::nullopt}});
inline constexpr auto reactive_energy_quadrant_iii = make_attribute_block(
	energy_quadrant_iii_layout, Period::Basic, DataType::uint64,
	std::array{Key{Id::ReactiveEnergyQuadrantIIIA, std::nullopt},
		Key{Id::ReactiveEnergyQuadrantIIIB, std::nullopt},
		Key{Id::ReactiveEnergyQuadrantIIIC, std::nullopt},
		Key{Id::ReactiveEnergyQuadrantIIITotal, std::nullopt}});
inline constexpr auto reactive_energy_quadrant_iv = make_attribute_block(
	energy_quadrant_iv_layout, Period::Basic, DataType::uint64,
	std::array{Key{Id::ReactiveEnergyQuadrantIVA, std::nullopt},
		Key{Id::ReactiveEnergyQuadrantIVB, std::nullopt},
		Key{Id::ReactiveEnergyQuadrantIVC, std::nullopt},
		Key{Id::ReactiveEnergyQuadrantIVTotal, std::nullopt}});

inline constexpr auto energy_metadata = make_special_block(
	energy_metadata_layout,
	std::array{
		SpecialDefinition{SpecialRegister::energy_session_id,
			DataType::uint64, Period::Basic},
		SpecialDefinition{SpecialRegister::energy_reset_epoch,
			DataType::uint64, Period::Basic},
		SpecialDefinition{SpecialRegister::energy_last_sample,
			DataType::uint64, Period::Basic},
		SpecialDefinition{SpecialRegister::energy_accepted_samples,
			DataType::uint64, Period::Basic},
		SpecialDefinition{SpecialRegister::energy_skipped_samples,
			DataType::uint64, Period::Basic},
		SpecialDefinition{SpecialRegister::energy_accepted_blocks,
			DataType::uint32, Period::Basic},
		SpecialDefinition{SpecialRegister::energy_skipped_blocks,
			DataType::uint32, Period::Basic},
		SpecialDefinition{SpecialRegister::energy_flags,
			DataType::uint32, Period::Basic},
		SpecialDefinition{SpecialRegister::energy_quality_mask,
			DataType::uint64, Period::Basic},
	});

inline constexpr RegisterBlock demand_current_layout{
	blocks::minute_10.function, 0x2000, 0x0010, "demand.current"};
inline constexpr RegisterBlock demand_import_peak_layout{
	blocks::minute_10.function, 0x2010, 0x0010,
	"demand.import_peak"};
inline constexpr RegisterBlock demand_export_peak_layout{
	blocks::minute_10.function, 0x2020, 0x0010,
	"demand.export_peak"};
inline constexpr RegisterBlock demand_metadata_layout{
	blocks::minute_10.function, 0x2030, 0x001e,
	"demand.metadata"};
inline constexpr RegisterBlock demand_anchor_layout{
	blocks::minute_10.function, 0x2050, 0x0020,
	"demand.peak_anchors"};

inline constexpr auto current_demand = make_attribute_block(
	demand_current_layout, Period::Demand, DataType::int64,
	std::array{Key{Id::CurrentActiveDemandA, std::nullopt},
		Key{Id::CurrentActiveDemandB, std::nullopt},
		Key{Id::CurrentActiveDemandC, std::nullopt},
		Key{Id::CurrentActiveDemandTotal, std::nullopt}});
inline constexpr auto import_demand_peak = make_attribute_block(
	demand_import_peak_layout, Period::Demand, DataType::uint64,
	std::array{Key{Id::ImportDemandPeakA, std::nullopt},
		Key{Id::ImportDemandPeakB, std::nullopt},
		Key{Id::ImportDemandPeakC, std::nullopt},
		Key{Id::ImportDemandPeakTotal, std::nullopt}});
inline constexpr auto export_demand_peak = make_attribute_block(
	demand_export_peak_layout, Period::Demand, DataType::uint64,
	std::array{Key{Id::ExportDemandPeakA, std::nullopt},
		Key{Id::ExportDemandPeakB, std::nullopt},
		Key{Id::ExportDemandPeakC, std::nullopt},
		Key{Id::ExportDemandPeakTotal, std::nullopt}});

inline constexpr auto demand_metadata = make_special_block(
	demand_metadata_layout,
	std::array{
		SpecialDefinition{SpecialRegister::demand_session_id,
			DataType::uint64, Period::Demand},
		SpecialDefinition{SpecialRegister::demand_reset_epoch,
			DataType::uint64, Period::Demand},
		SpecialDefinition{SpecialRegister::demand_last_sample,
			DataType::uint64, Period::Demand},
		SpecialDefinition{SpecialRegister::demand_interval_anchor_sample,
			DataType::uint64, Period::Demand},
		SpecialDefinition{SpecialRegister::demand_source_interval_count,
			DataType::uint32, Period::Demand},
		SpecialDefinition{SpecialRegister::demand_source_status,
			DataType::uint32, Period::Demand},
		SpecialDefinition{SpecialRegister::demand_method,
			DataType::uint16, Period::Demand},
		SpecialDefinition{SpecialRegister::demand_window_seconds,
			DataType::uint32, Period::Demand},
		SpecialDefinition{SpecialRegister::demand_update_seconds,
			DataType::uint32, Period::Demand},
		SpecialDefinition{SpecialRegister::demand_profile_generation,
			DataType::uint32, Period::Demand},
		SpecialDefinition{SpecialRegister::demand_flags,
			DataType::uint32, Period::Demand},
		SpecialDefinition{SpecialRegister::demand_quality_mask,
			DataType::uint16, Period::Demand},
	});

inline constexpr auto demand_peak_anchors = make_special_block(
	demand_anchor_layout,
	std::array{
		SpecialDefinition{SpecialRegister::demand_import_peak_sample_a,
			DataType::uint64, Period::Demand},
		SpecialDefinition{SpecialRegister::demand_import_peak_sample_b,
			DataType::uint64, Period::Demand},
		SpecialDefinition{SpecialRegister::demand_import_peak_sample_c,
			DataType::uint64, Period::Demand},
		SpecialDefinition{SpecialRegister::demand_import_peak_sample_total,
			DataType::uint64, Period::Demand},
		SpecialDefinition{SpecialRegister::demand_export_peak_sample_a,
			DataType::uint64, Period::Demand},
		SpecialDefinition{SpecialRegister::demand_export_peak_sample_b,
			DataType::uint64, Period::Demand},
		SpecialDefinition{SpecialRegister::demand_export_peak_sample_c,
			DataType::uint64, Period::Demand},
		SpecialDefinition{SpecialRegister::demand_export_peak_sample_total,
			DataType::uint64, Period::Demand},
	});

/** One sorted source of truth used by runtime, tests, and map documentation. */
inline constexpr auto register_map = concat(
	metadata, basic_frequency, basic_voltage_ln, basic_current, basic_status,
	active_import_energy, active_export_energy, apparent_energy,
	reactive_energy_quadrant_i, reactive_energy_quadrant_ii,
	reactive_energy_quadrant_iii, reactive_energy_quadrant_iv,
	energy_metadata, current_demand, import_demand_peak, export_demand_peak,
	demand_metadata, demand_peak_anchors);

inline constexpr std::array basic_published_attributes{
	Key{Id::Frequency, std::nullopt},
	Key{Id::VanRms, std::nullopt},
	Key{Id::VbnRms, std::nullopt},
	Key{Id::VcnRms, std::nullopt},
	Key{Id::IaRms, std::nullopt},
	Key{Id::IbRms, std::nullopt},
	Key{Id::IcRms, std::nullopt},
	Key{Id::InRms, std::nullopt},
};

inline constexpr auto energy_published_attributes = concat(
	active_import_energy, active_export_energy, apparent_energy,
	reactive_energy_quadrant_i, reactive_energy_quadrant_ii,
	reactive_energy_quadrant_iii, reactive_energy_quadrant_iv);

inline constexpr auto demand_published_attributes = concat(
	current_demand, import_demand_peak, export_demand_peak);

inline constexpr auto published_measurement_attributes = concat(
	basic_published_attributes,
	std::array{Key{Id::ActiveImportEnergyA, std::nullopt},
		Key{Id::ActiveImportEnergyB, std::nullopt},
		Key{Id::ActiveImportEnergyC, std::nullopt},
		Key{Id::ActiveImportEnergyTotal, std::nullopt},
		Key{Id::ActiveExportEnergyA, std::nullopt},
		Key{Id::ActiveExportEnergyB, std::nullopt},
		Key{Id::ActiveExportEnergyC, std::nullopt},
		Key{Id::ActiveExportEnergyTotal, std::nullopt},
		Key{Id::ApparentEnergyA, std::nullopt},
		Key{Id::ApparentEnergyB, std::nullopt},
		Key{Id::ApparentEnergyC, std::nullopt},
		Key{Id::ApparentEnergyTotal, std::nullopt},
		Key{Id::ReactiveEnergyQuadrantIA, std::nullopt},
		Key{Id::ReactiveEnergyQuadrantIB, std::nullopt},
		Key{Id::ReactiveEnergyQuadrantIC, std::nullopt},
		Key{Id::ReactiveEnergyQuadrantITotal, std::nullopt},
		Key{Id::ReactiveEnergyQuadrantIIA, std::nullopt},
		Key{Id::ReactiveEnergyQuadrantIIB, std::nullopt},
		Key{Id::ReactiveEnergyQuadrantIIC, std::nullopt},
		Key{Id::ReactiveEnergyQuadrantIITotal, std::nullopt},
		Key{Id::ReactiveEnergyQuadrantIIIA, std::nullopt},
		Key{Id::ReactiveEnergyQuadrantIIIB, std::nullopt},
		Key{Id::ReactiveEnergyQuadrantIIIC, std::nullopt},
		Key{Id::ReactiveEnergyQuadrantIIITotal, std::nullopt},
		Key{Id::ReactiveEnergyQuadrantIVA, std::nullopt},
		Key{Id::ReactiveEnergyQuadrantIVB, std::nullopt},
		Key{Id::ReactiveEnergyQuadrantIVC, std::nullopt},
		Key{Id::ReactiveEnergyQuadrantIVTotal, std::nullopt},
		Key{Id::CurrentActiveDemandA, std::nullopt},
		Key{Id::CurrentActiveDemandB, std::nullopt},
		Key{Id::CurrentActiveDemandC, std::nullopt},
		Key{Id::CurrentActiveDemandTotal, std::nullopt},
		Key{Id::ImportDemandPeakA, std::nullopt},
		Key{Id::ImportDemandPeakB, std::nullopt},
		Key{Id::ImportDemandPeakC, std::nullopt},
		Key{Id::ImportDemandPeakTotal, std::nullopt},
		Key{Id::ExportDemandPeakA, std::nullopt},
		Key{Id::ExportDemandPeakB, std::nullopt},
		Key{Id::ExportDemandPeakC, std::nullopt},
		Key{Id::ExportDemandPeakTotal, std::nullopt}});

static_assert(validate_register_map(register_blocks, register_map),
	"MSAP1 Modbus blocks or generated register definitions are invalid");

} // namespace msap1::modbus::schema
