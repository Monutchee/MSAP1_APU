#include "msap1/modbus/modbus_register_map.hpp"

#include "mnc/MeterDataProvider/attributes/meter_attribute_set.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace msap1::modbus {
namespace {

using mnc::meter::MeterAttributeKey;
using mnc::meter::MeterAttributeValue;
using mnc::meter::MeterSnapshot;
using mnc::meter::MeterSnapshotRequest;
using mnc::meter::MeasurementPeriod;
using mnc::meter::ReadingQuality;
using mnc::modbus::FunctionCode;
using schema::MeasurementSource;
using schema::SpecialSource;

const RegisterDefinition *definition(FunctionCode function,
	std::uint16_t address)
{
	return schema::find_definition(schema::register_map, function, address);
}

const MeterAttributeValue *reading(const MeterSnapshot &snapshot,
	MeterAttributeKey attribute)
{
	const auto found = std::ranges::find_if(snapshot.values,
		[attribute](const auto &value) {
			return value.attribute == attribute;
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

std::vector<std::uint16_t> encode_measurement(
	const RegisterDefinition &entry, const MeasurementSource &source,
	const std::optional<MeterSnapshot> &snapshot)
{
	const auto *value = snapshot ? reading(*snapshot, source.attribute) : nullptr;
	const auto available = value && value->quality == ReadingQuality::Valid;
	switch (entry.type) {
	case DataType::float32:
		return mnc::modbus::encode_float(engineering_value(value));
	case DataType::uint16:
		return mnc::modbus::encode_u16(available
			? static_cast<std::uint16_t>(value->value) : 0);
	case DataType::uint32:
		return mnc::modbus::encode_u32(available
			? static_cast<std::uint32_t>(value->value) : 0);
	case DataType::int32:
		return mnc::modbus::encode_i32(available
			? static_cast<std::int32_t>(value->value) : 0);
	case DataType::uint64:
		return mnc::modbus::encode_u64(available
			? static_cast<std::uint64_t>(value->value) : 0);
	}
	throw std::logic_error("unsupported MSAP1 Modbus measurement datatype");
}

std::vector<std::uint16_t> encode_special(const SpecialSource &source,
	const std::optional<MeterSnapshot> &snapshot)
{
	switch (source.field) {
	case SpecialRegister::quality_mask: {
		std::uint16_t mask = 0;
		if (snapshot) {
			for (std::size_t index = 0;
			     index < schema::published_measurement_attributes.size();
			     ++index) {
				const auto *value = reading(*snapshot,
					schema::published_measurement_attributes[index]);
				if (value && value->quality == ReadingQuality::Valid)
					mask |= static_cast<std::uint16_t>(1u << index);
			}
		}
		return {mask};
	}
	case SpecialRegister::period:
		return {static_cast<std::uint16_t>(snapshot
			? snapshot->period
			: source.period.value_or(MeasurementPeriod::Basic))};
	case SpecialRegister::source_sequence:
		return mnc::modbus::encode_u32(snapshot
			? static_cast<std::uint32_t>(snapshot->sequence) : 0);
	case SpecialRegister::configuration_generation:
		return mnc::modbus::encode_u32(snapshot
			? snapshot->configuration_generation : 0);
	case SpecialRegister::map_version:
		return {schema::register_map_version};
	case SpecialRegister::word_order_marker:
		return {0x1234};
	case SpecialRegister::attribute_count:
		return {static_cast<std::uint16_t>(
			schema::published_measurement_attributes.size())};
	}
	throw std::logic_error("unhandled MSAP1 Modbus special register");
}

std::vector<std::uint16_t> encode(const RegisterDefinition &entry,
	const std::optional<MeterSnapshot> &snapshot)
{
	return std::visit(
		[&](const auto &source) -> std::vector<std::uint16_t> {
			using Source = std::decay_t<decltype(source)>;
			if constexpr (std::is_same_v<Source, MeasurementSource>)
				return encode_measurement(entry, source, snapshot);
			else
				return encode_special(source, snapshot);
		},
		entry.source);
}

struct SnapshotSelection {
	std::optional<MeasurementPeriod> period;
	mnc::meter::MeterAttributeSet attributes;
	bool incompatible_periods = false;
};

void include_period(SnapshotSelection &selection, MeasurementPeriod period)
{
	if (selection.period && *selection.period != period)
		selection.incompatible_periods = true;
	else
		selection.period = period;
}

SnapshotSelection snapshot_selection(FunctionCode function,
	std::uint16_t address, std::uint32_t end)
{
	SnapshotSelection selection;
	const RegisterDefinition *previous = nullptr;
	for (std::uint32_t current = address; current < end; ++current) {
		const auto *entry = definition(function,
			static_cast<std::uint16_t>(current));
		if (entry == previous)
			continue;
		previous = entry;
		std::visit(
			[&](const auto &source) {
				using Source = std::decay_t<decltype(source)>;
				if constexpr (std::is_same_v<Source, MeasurementSource>) {
					include_period(selection, source.period);
					selection.attributes.add(source.attribute);
				} else if (source.period) {
					include_period(selection, *source.period);
					if (source.field == SpecialRegister::quality_mask)
						for (const auto attribute :
						     schema::published_measurement_attributes)
							selection.attributes.add(attribute);
				}
			},
			entry->source);
	}
	return selection;
}

} // namespace

mnc::modbus::RegisterReadResult Msap1RegisterBank::read(
	FunctionCode function, std::uint16_t address, std::uint16_t count) const
{
	if (!mnc::modbus::is_register_read(function))
		return {mnc::modbus::ExceptionCode::illegal_function, {}};
	if (count == 0)
		return {mnc::modbus::ExceptionCode::illegal_data_value, {}};
	const auto end = static_cast<std::uint32_t>(address) + count;
	if (end > 0x10000u)
		return {mnc::modbus::ExceptionCode::illegal_data_address, {}};

	/* Validate every requested 16-bit register before entering IPC. Reserved
	 * gaps remain unavailable until a future map version defines them. */
	for (std::uint32_t current = address; current < end; ++current) {
		if (!definition(function, static_cast<std::uint16_t>(current)))
			return {mnc::modbus::ExceptionCode::illegal_data_address, {}};
	}

	const auto selection = snapshot_selection(function, address, end);
	if (selection.incompatible_periods)
		return {mnc::modbus::ExceptionCode::illegal_data_address, {}};

	std::optional<MeterSnapshot> snapshot;
	if (selection.period) {
		MeterSnapshotRequest request;
		request.period = *selection.period;
		request.attributes = selection.attributes.values();
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
	for (std::uint32_t current = address; current < end; ++current) {
		const auto *entry = definition(function,
			static_cast<std::uint16_t>(current));
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
