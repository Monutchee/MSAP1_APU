#include "msap1/modbus/modbus_register_map.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace msap1::modbus {
namespace {

using mnc::meter::MeterAttributeId;
using mnc::meter::MeterAttributeKey;
using mnc::meter::MeterAttributeValue;
using mnc::meter::ReadingQuality;
using mnc::modbus::FunctionCode;

/* Input registers are intentionally contiguous. This makes common SCADA
 * block reads efficient while this table remains the reviewable protocol
 * contract. 32-bit values occupy address/address+1, high word first. */
constexpr std::array register_definitions{
	RegisterDefinition{FunctionCode::read_input_registers, 0, 2, DataType::float32,
		MeterField::frequency},
	RegisterDefinition{FunctionCode::read_input_registers, 2, 2, DataType::float32,
		MeterField::voltage_ln_a},
	RegisterDefinition{FunctionCode::read_input_registers, 4, 2, DataType::float32,
		MeterField::voltage_ln_b},
	RegisterDefinition{FunctionCode::read_input_registers, 6, 2, DataType::float32,
		MeterField::voltage_ln_c},
	RegisterDefinition{FunctionCode::read_input_registers, 8, 2, DataType::float32,
		MeterField::current_a},
	RegisterDefinition{FunctionCode::read_input_registers, 10, 2, DataType::float32,
		MeterField::current_b},
	RegisterDefinition{FunctionCode::read_input_registers, 12, 2, DataType::float32,
		MeterField::current_c},
	RegisterDefinition{FunctionCode::read_input_registers, 14, 2, DataType::float32,
		MeterField::current_neutral},
	RegisterDefinition{FunctionCode::read_input_registers, 16, 1, DataType::uint16,
		MeterField::quality_mask},
	RegisterDefinition{FunctionCode::read_input_registers, 17, 1, DataType::uint16,
		MeterField::period},
	RegisterDefinition{FunctionCode::read_input_registers, 18, 2, DataType::uint32,
		MeterField::source_sequence},
	RegisterDefinition{FunctionCode::read_input_registers, 20, 2, DataType::uint32,
		MeterField::configuration_generation},
	RegisterDefinition{FunctionCode::read_holding_registers, 0, 1, DataType::uint16,
		MeterField::map_version},
	RegisterDefinition{FunctionCode::read_holding_registers, 1, 1, DataType::uint16,
		MeterField::word_order_marker},
	RegisterDefinition{FunctionCode::read_holding_registers, 2, 1, DataType::uint16,
		MeterField::attribute_count},
};

consteval std::uint16_t expected_words(DataType type)
{
	switch (type) {
	case DataType::uint16: return 1;
	case DataType::uint32:
	case DataType::float32: return 2;
	}
	return 0;
}

consteval bool valid_register_contract()
{
	for (std::size_t index = 0; index < register_definitions.size(); ++index) {
		const auto &entry = register_definitions[index];
		if (entry.words == 0 || entry.words != expected_words(entry.type) ||
		    static_cast<std::uint32_t>(entry.address) + entry.words > 0x10000u)
			return false;
		for (std::size_t other = index + 1;
		     other < register_definitions.size(); ++other) {
			const auto &candidate = register_definitions[other];
			if (candidate.function != entry.function)
				continue;
			const auto entry_end = static_cast<std::uint32_t>(entry.address) +
				entry.words;
			const auto candidate_end =
				static_cast<std::uint32_t>(candidate.address) +
				candidate.words;
			if (entry.address < candidate_end &&
			    candidate.address < entry_end)
				return false;
		}
	}
	return true;
}

static_assert(valid_register_contract(),
	"MSAP1 Modbus register definitions overlap or have an invalid width");

constexpr std::array measurement_attributes{
	MeterAttributeId::Frequency,
	MeterAttributeId::VanRms,
	MeterAttributeId::VbnRms,
	MeterAttributeId::VcnRms,
	MeterAttributeId::IaRms,
	MeterAttributeId::IbRms,
	MeterAttributeId::IcRms,
	MeterAttributeId::InRms,
};

const RegisterDefinition *definition(FunctionCode function, std::uint16_t address)
{
	const auto found = std::ranges::find_if(register_definitions,
		[=](const auto &entry) {
			return entry.function == function && address >= entry.address &&
			       address < entry.address + entry.words;
		});
	return found == register_definitions.end() ? nullptr : &*found;
}

std::optional<MeterAttributeId> attribute(MeterField field)
{
	switch (field) {
	case MeterField::frequency: return MeterAttributeId::Frequency;
	case MeterField::voltage_ln_a: return MeterAttributeId::VanRms;
	case MeterField::voltage_ln_b: return MeterAttributeId::VbnRms;
	case MeterField::voltage_ln_c: return MeterAttributeId::VcnRms;
	case MeterField::current_a: return MeterAttributeId::IaRms;
	case MeterField::current_b: return MeterAttributeId::IbRms;
	case MeterField::current_c: return MeterAttributeId::IcRms;
	case MeterField::current_neutral: return MeterAttributeId::InRms;
	default: return std::nullopt;
	}
}

const MeterAttributeValue *reading(const mnc::meter::MeterSnapshot &snapshot,
	MeterAttributeId id)
{
	const auto found = std::ranges::find_if(snapshot.values,
		[id](const auto &value) {
			return value.attribute.id == id && !value.attribute.index;
		});
	return found == snapshot.values.end() ? nullptr : &*found;
}

float engineering_value(const MeterAttributeValue *value)
{
	if (!value || value->quality != ReadingQuality::Valid)
		return std::numeric_limits<float>::quiet_NaN();
	switch (value->unit) {
	case mnc::meter::MeterUnit::MilliHertz:
		return static_cast<float>(value->value) / 1'000.0f;
	case mnc::meter::MeterUnit::MicroVolts:
	case mnc::meter::MeterUnit::MicroAmperes:
		return static_cast<float>(value->value) / 1'000'000.0f;
	}
	return std::numeric_limits<float>::quiet_NaN();
}

std::vector<std::uint16_t> encode(const RegisterDefinition &entry,
	const std::optional<mnc::meter::MeterSnapshot> &snapshot)
{
	if (const auto id = attribute(entry.field)) {
		return mnc::modbus::encode_float(snapshot
			? engineering_value(reading(*snapshot, *id))
			: std::numeric_limits<float>::quiet_NaN());
	}
	switch (entry.field) {
	case MeterField::quality_mask: {
		std::uint16_t mask = 0;
		if (snapshot)
			for (std::size_t index = 0; index < measurement_attributes.size(); ++index) {
				const auto *value = reading(*snapshot,
					measurement_attributes[index]);
				if (value && value->quality == ReadingQuality::Valid)
					mask |= static_cast<std::uint16_t>(1u << index);
			}
		return {mask};
	}
	case MeterField::period:
		return {static_cast<std::uint16_t>(snapshot
			? snapshot->period : mnc::meter::MeasurementPeriod::Basic)};
	case MeterField::source_sequence:
		return mnc::modbus::encode_u32(snapshot
			? static_cast<std::uint32_t>(snapshot->sequence) : 0);
	case MeterField::configuration_generation:
		return mnc::modbus::encode_u32(snapshot
			? snapshot->configuration_generation : 0);
	case MeterField::map_version: return {register_map_version};
	case MeterField::word_order_marker: return {0x1234};
	case MeterField::attribute_count:
		return {static_cast<std::uint16_t>(measurement_attributes.size())};
	default:
		throw std::logic_error("unhandled MSAP1 Modbus register field");
	}
}

} // namespace

std::span<const RegisterDefinition> Msap1RegisterBank::definitions()
{
	return register_definitions;
}

mnc::modbus::RegisterReadResult Msap1RegisterBank::read(
	FunctionCode function, std::uint16_t address, std::uint16_t count) const
{
	if (!mnc::modbus::is_register_read(function))
		return {mnc::modbus::ExceptionCode::illegal_function, {}};
	if (count == 0 || static_cast<std::uint32_t>(address) + count > 0x10000u)
		return {mnc::modbus::ExceptionCode::illegal_data_value, {}};

	std::optional<mnc::meter::MeterSnapshot> snapshot;
	if (function == FunctionCode::read_input_registers) {
		mnc::meter::MeterSnapshotRequest request;
		request.period = mnc::meter::MeasurementPeriod::Basic;
		for (const auto id : measurement_attributes)
			request.attributes.push_back(MeterAttributeKey{id, std::nullopt});
		try {
			snapshot = provider_.latest(request);
		} catch (...) {
			return {mnc::modbus::ExceptionCode::server_device_failure, {}};
		}
	}

	std::vector<std::uint16_t> result;
	result.reserve(count);
	const RegisterDefinition *cached = nullptr;
	std::vector<std::uint16_t> encoded;
	for (std::uint32_t current = address;
	     current < static_cast<std::uint32_t>(address) + count; ++current) {
		const auto *entry = definition(function, static_cast<std::uint16_t>(current));
		if (!entry)
			return {mnc::modbus::ExceptionCode::illegal_data_address, {}};
		if (entry != cached) {
			encoded = encode(*entry, snapshot);
			cached = entry;
		}
		const auto offset = current - entry->address;
		if (offset >= encoded.size())
			return {mnc::modbus::ExceptionCode::server_device_failure, {}};
		result.push_back(encoded[offset]);
	}
	return {mnc::modbus::ExceptionCode::none, std::move(result)};
}

mnc::modbus::ExceptionCode Msap1RegisterBank::write_single(
	std::uint16_t, std::uint16_t)
{
	/* FC06 is implemented by the common protocol engine. The initial product
	 * map deliberately exposes no writable control register. */
	return mnc::modbus::ExceptionCode::illegal_data_address;
}

mnc::modbus::ExceptionCode Msap1RegisterBank::write_multiple(
	std::uint16_t, std::span<const std::uint16_t>)
{
	return mnc::modbus::ExceptionCode::illegal_data_address;
}

} // namespace msap1::modbus
