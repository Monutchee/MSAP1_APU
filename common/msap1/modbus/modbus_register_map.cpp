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
	case mnc::meter::MeterUnit::Picowatts:
	case mnc::meter::MeterUnit::PicoVoltAmperes:
		/* pico -> base units (W / VA). */
		return static_cast<float>(value->value) / 1e12f;
	case mnc::meter::MeterUnit::PowerFactorMillionths:
		return static_cast<float>(value->value) / 1'000'000.0f;
	case mnc::meter::MeterUnit::Picovars:
		/* pico -> base units (var). */
		return static_cast<float>(value->value) / 1e12f;
	case mnc::meter::MeterUnit::Millidegrees:
		/* The PL publishes the 0..359.999-degree convention directly. */
		return static_cast<float>(value->value) / 1000.0f;
	case mnc::meter::MeterUnit::RatioMillionths:
		/* millionths -> percent (the human unit for unbalance). */
		return static_cast<float>(value->value) / 10000.0f;
	case mnc::meter::MeterUnit::MicroWattHours:
	case mnc::meter::MeterUnit::MicroVarHours:
	case mnc::meter::MeterUnit::MicroVoltAmpereHours:
	case mnc::meter::MeterUnit::MicroWatts:
		return static_cast<float>(value->value);
	case mnc::meter::MeterUnit::CrestTenThousandths:
		return static_cast<float>(value->value) / 10'000.0f;
	case mnc::meter::MeterUnit::CategoricalCode:
		return static_cast<float>(value->value);
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
	case DataType::int64:
		return mnc::modbus::encode_i64(available ? value->value : 0);
	}
	throw std::logic_error("unsupported MSAP1 Modbus measurement datatype");
}

std::vector<std::uint16_t> encode_special(const SpecialSource &source,
	const std::optional<MeterSnapshot> &snapshot)
{
	const auto energy = snapshot && snapshot->energy
		? &*snapshot->energy : nullptr;
	const auto demand = snapshot && snapshot->demand
		? &*snapshot->demand : nullptr;
	const auto valid_mask = [&](const auto &definitions) {
		std::uint64_t mask = 0;
		if (!snapshot)
			return mask;
		for (std::size_t index = 0; index < definitions.size(); ++index) {
			const auto &measurement =
				std::get<MeasurementSource>(definitions[index].source);
			const auto *value = reading(*snapshot, measurement.attribute);
			if (value && value->quality == ReadingQuality::Valid)
				mask |= std::uint64_t{1} << index;
		}
		return mask;
	};
	switch (source.field) {
	case SpecialRegister::quality_mask: {
		std::uint16_t mask = 0;
		if (snapshot) {
			for (std::size_t index = 0;
			     index < schema::basic_published_attributes.size();
			     ++index) {
				const auto *value = reading(*snapshot,
					schema::basic_published_attributes[index]);
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
	case SpecialRegister::energy_session_id:
		return mnc::modbus::encode_u64(energy ? energy->session_id : 0);
	case SpecialRegister::energy_reset_epoch:
		return mnc::modbus::encode_u64(energy ? energy->reset_epoch : 0);
	case SpecialRegister::energy_last_sample:
		return mnc::modbus::encode_u64(energy ? energy->last_sample_index : 0);
	case SpecialRegister::energy_accepted_samples:
		return mnc::modbus::encode_u64(energy ? energy->accepted_samples : 0);
	case SpecialRegister::energy_skipped_samples:
		return mnc::modbus::encode_u64(energy ? energy->skipped_samples : 0);
	case SpecialRegister::energy_accepted_blocks:
		return mnc::modbus::encode_u32(energy ? energy->accepted_blocks : 0);
	case SpecialRegister::energy_skipped_blocks:
		return mnc::modbus::encode_u32(energy ? energy->skipped_blocks : 0);
	case SpecialRegister::energy_flags:
		return mnc::modbus::encode_u32(energy
			? static_cast<std::uint32_t>(energy->saturated) |
				(static_cast<std::uint32_t>(energy->incomplete_input) << 1) |
				(static_cast<std::uint32_t>(energy->discontinuity) << 2)
			: 0);
	case SpecialRegister::energy_quality_mask:
		return mnc::modbus::encode_u64(
			valid_mask(schema::energy_published_attributes));
	case SpecialRegister::demand_session_id:
		return mnc::modbus::encode_u64(demand ? demand->session_id : 0);
	case SpecialRegister::demand_reset_epoch:
		return mnc::modbus::encode_u64(demand ? demand->peak_reset_epoch : 0);
	case SpecialRegister::demand_last_sample:
		return mnc::modbus::encode_u64(demand ? demand->last_sample_index : 0);
	case SpecialRegister::demand_interval_anchor_sample:
		return mnc::modbus::encode_u64(
			demand ? demand->interval_anchor_sample : 0);
	case SpecialRegister::demand_source_interval_count:
		return mnc::modbus::encode_u32(
			demand ? demand->source_interval_count : 0);
	case SpecialRegister::demand_source_status:
		return mnc::modbus::encode_u32(demand ? demand->source_status : 0);
	case SpecialRegister::demand_method:
		return {static_cast<std::uint16_t>(demand ? demand->method : 0)};
	case SpecialRegister::demand_window_seconds:
		return mnc::modbus::encode_u32(demand ? demand->window_seconds : 0);
	case SpecialRegister::demand_update_seconds:
		return mnc::modbus::encode_u32(demand ? demand->update_seconds : 0);
	case SpecialRegister::demand_profile_generation:
		return mnc::modbus::encode_u32(
			demand ? demand->profile_generation : 0);
	case SpecialRegister::demand_flags:
		return mnc::modbus::encode_u32(demand
			? static_cast<std::uint32_t>(demand->time_aligned) |
				(static_cast<std::uint32_t>(demand->contaminated) << 1) |
				(static_cast<std::uint32_t>(demand->boundary_valid) << 2) |
				(static_cast<std::uint32_t>(demand->saturated) << 3) |
				(static_cast<std::uint32_t>(demand->incomplete_input) << 4)
			: 0);
	case SpecialRegister::demand_quality_mask:
		return {static_cast<std::uint16_t>(
			valid_mask(schema::demand_published_attributes))};
	case SpecialRegister::demand_import_peak_sample_a:
	case SpecialRegister::demand_import_peak_sample_b:
	case SpecialRegister::demand_import_peak_sample_c:
	case SpecialRegister::demand_import_peak_sample_total: {
		const auto index = static_cast<std::size_t>(source.field) -
			static_cast<std::size_t>(
				SpecialRegister::demand_import_peak_sample_a);
		return mnc::modbus::encode_u64(
			demand ? demand->import_peak_samples[index] : 0);
	}
	case SpecialRegister::demand_export_peak_sample_a:
	case SpecialRegister::demand_export_peak_sample_b:
	case SpecialRegister::demand_export_peak_sample_c:
	case SpecialRegister::demand_export_peak_sample_total: {
		const auto index = static_cast<std::size_t>(source.field) -
			static_cast<std::size_t>(
				SpecialRegister::demand_export_peak_sample_a);
		return mnc::modbus::encode_u64(
			demand ? demand->export_peak_samples[index] : 0);
	}
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
						     schema::basic_published_attributes)
							selection.attributes.add(attribute);
					else if (source.field ==
						 SpecialRegister::energy_quality_mask)
						for (const auto &definition :
						     schema::energy_published_attributes)
							selection.attributes.add(std::get<
								MeasurementSource>(definition.source).attribute);
					else if (source.field ==
						 SpecialRegister::demand_quality_mask)
						for (const auto &definition :
						     schema::demand_published_attributes)
							selection.attributes.add(std::get<
								MeasurementSource>(definition.source).attribute);
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
