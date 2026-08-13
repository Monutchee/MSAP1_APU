#pragma once

#include "mnc/MeterDataProvider/attributes/meter_attribute.hpp"
#include "mnc/modbus/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>

namespace msap1::modbus::schema {

/** Storage representation of one logical value in the Modbus register map. */
enum class DataType : std::uint8_t {
	uint16,
	uint32,
	int32,
	float32,
	uint64,
};

/** Product registers which are not backed directly by a meter attribute. */
enum class SpecialRegister : std::uint8_t {
	map_version,
	word_order_marker,
	attribute_count,
	quality_mask,
	period,
	source_sequence,
	configuration_generation,
};

/** A measurement value selected from one independently calculated period. */
struct MeasurementSource {
	mnc::meter::MeasurementPeriod period =
		mnc::meter::MeasurementPeriod::Basic;
	mnc::meter::MeterAttributeKey attribute;

	auto operator<=>(const MeasurementSource &) const = default;
};

/**
 * Source for status/provenance values.
 *
 * `period` is present when producing the value requires a coherent meter
 * snapshot. Static holding-register metadata deliberately leaves it empty.
 */
struct SpecialSource {
	SpecialRegister field = SpecialRegister::map_version;
	std::optional<mnc::meter::MeasurementPeriod> period;

	auto operator<=>(const SpecialSource &) const = default;
};

using RegisterSource = std::variant<MeasurementSource, SpecialSource>;

/** Stable reserved region in one Modbus function/address space. */
struct RegisterBlock {
	mnc::modbus::FunctionCode function =
		mnc::modbus::FunctionCode::read_input_registers;
	std::uint16_t base = 0;
	std::uint32_t size = 0;
	std::string_view name;
};

/** One encoded logical value in the flattened, sorted register map. */
struct RegisterDefinition {
	mnc::modbus::FunctionCode function =
		mnc::modbus::FunctionCode::read_input_registers;
	std::uint16_t address = 0;
	std::uint16_t words = 0;
	DataType type = DataType::uint16;
	RegisterSource source = SpecialSource{};
	/** Explicit protocol aliases may repeat a logical source. */
	bool compatibility_alias = false;
};

struct SpecialDefinition {
	SpecialRegister field = SpecialRegister::map_version;
	DataType type = DataType::uint16;
	std::optional<mnc::meter::MeasurementPeriod> period;
};

[[nodiscard]] constexpr std::uint16_t register_width(DataType type) noexcept
{
	switch (type) {
	case DataType::uint16: return 1;
	case DataType::uint32:
	case DataType::int32:
	case DataType::float32: return 2;
	case DataType::uint64: return 4;
	}
	return 0;
}

[[nodiscard]] constexpr std::uint32_t block_end(
	const RegisterBlock &block) noexcept
{
	return static_cast<std::uint32_t>(block.base) + block.size;
}

[[nodiscard]] constexpr std::uint32_t definition_end(
	const RegisterDefinition &definition) noexcept
{
	return static_cast<std::uint32_t>(definition.address) + definition.words;
}

/** Generate a dense group while keeping its absolute block base explicit. */
template<std::size_t N>
[[nodiscard]] consteval std::array<RegisterDefinition, N>
make_attribute_block(const RegisterBlock &block,
	mnc::meter::MeasurementPeriod period, DataType type,
	const std::array<mnc::meter::MeterAttributeKey, N> &attributes)
{
	std::array<RegisterDefinition, N> result{};
	const auto words = register_width(type);
	for (std::size_t index = 0; index < N; ++index) {
		result[index] = RegisterDefinition{
			.function = block.function,
			.address = static_cast<std::uint16_t>(
				static_cast<std::uint32_t>(block.base) + index * words),
			.words = words,
			.type = type,
			.source = MeasurementSource{period, attributes[index]},
		};
	}
	return result;
}

/** Generate a simple indexed family such as harmonic orders 1 through 50. */
template<std::uint16_t FirstIndex, std::uint16_t LastIndex>
[[nodiscard]] consteval auto make_indexed_block(const RegisterBlock &block,
	mnc::meter::MeasurementPeriod period,
	mnc::meter::MeterAttributeId attribute, DataType type)
{
	static_assert(LastIndex >= FirstIndex,
		"indexed Modbus register range is reversed");
	constexpr auto count =
		static_cast<std::size_t>(LastIndex - FirstIndex) + 1u;
	std::array<mnc::meter::MeterAttributeKey, count> attributes{};
	for (std::size_t index = 0; index < count; ++index)
		attributes[index] = {attribute,
			static_cast<std::uint16_t>(FirstIndex + index)};
	return make_attribute_block(block, period, type, attributes);
}

/** Generate compact explicit metadata/status fields inside a stable block. */
template<std::size_t N>
[[nodiscard]] consteval std::array<RegisterDefinition, N>
make_special_block(const RegisterBlock &block,
	const std::array<SpecialDefinition, N> &fields)
{
	std::array<RegisterDefinition, N> result{};
	std::uint32_t address = block.base;
	for (std::size_t index = 0; index < N; ++index) {
		const auto words = register_width(fields[index].type);
		result[index] = RegisterDefinition{
			.function = block.function,
			.address = static_cast<std::uint16_t>(address),
			.words = words,
			.type = fields[index].type,
			.source = SpecialSource{fields[index].field,
				fields[index].period},
		};
		address += words;
	}
	return result;
}

/** Flatten independently based groups without creating a global auto-packer. */
template<typename T, std::size_t... Sizes>
[[nodiscard]] consteval std::array<T, (Sizes + ... + 0u)> concat(
	const std::array<T, Sizes> &...groups)
{
	std::array<T, (Sizes + ... + 0u)> result{};
	std::size_t output = 0;
	auto append = [&](const auto &group) {
		for (const auto &entry : group)
			result[output++] = entry;
	};
	(append(groups), ...);
	return result;
}

[[nodiscard]] constexpr bool overlaps(std::uint32_t first_begin,
	std::uint32_t first_end, std::uint32_t second_begin,
	std::uint32_t second_end) noexcept
{
	return first_begin < second_end && second_begin < first_end;
}

template<std::size_t N>
[[nodiscard]] consteval bool validate_blocks(
	const std::array<RegisterBlock, N> &blocks)
{
	for (std::size_t index = 0; index < N; ++index) {
		const auto &block = blocks[index];
		if (block.size == 0 || block_end(block) > 0x10000u)
			return false;
		for (std::size_t other = index + 1; other < N; ++other) {
			const auto &candidate = blocks[other];
			if (candidate.function == block.function &&
			    overlaps(block.base, block_end(block), candidate.base,
				    block_end(candidate)))
				return false;
		}
	}
	return true;
}

[[nodiscard]] constexpr bool source_is_duplicate(
	const RegisterDefinition &first,
	const RegisterDefinition &second) noexcept
{
	const auto *first_measurement =
		std::get_if<MeasurementSource>(&first.source);
	const auto *second_measurement =
		std::get_if<MeasurementSource>(&second.source);
	return first_measurement && second_measurement &&
	       *first_measurement == *second_measurement &&
	       !first.compatibility_alias && !second.compatibility_alias;
}

[[nodiscard]] constexpr bool precedes(const RegisterDefinition &first,
	const RegisterDefinition &second) noexcept
{
	const auto first_function = static_cast<std::uint8_t>(first.function);
	const auto second_function = static_cast<std::uint8_t>(second.function);
	return first_function < second_function ||
	       (first_function == second_function &&
		first.address < second.address);
}

template<std::size_t BlockCount, std::size_t EntryCount>
[[nodiscard]] consteval bool validate_register_map(
	const std::array<RegisterBlock, BlockCount> &blocks,
	const std::array<RegisterDefinition, EntryCount> &entries)
{
	if (!validate_blocks(blocks))
		return false;
	for (std::size_t index = 0; index < EntryCount; ++index) {
		const auto &entry = entries[index];
		if (entry.words == 0 || entry.words != register_width(entry.type) ||
		    definition_end(entry) > 0x10000u)
			return false;

		std::size_t containing_blocks = 0;
		for (const auto &block : blocks) {
			if (block.function == entry.function &&
			    entry.address >= block.base &&
			    definition_end(entry) <= block_end(block))
				++containing_blocks;
		}
		if (containing_blocks != 1)
			return false;

		if (index > 0 && !precedes(entries[index - 1], entry))
			return false;
		for (std::size_t other = index + 1; other < EntryCount; ++other) {
			const auto &candidate = entries[other];
			if (candidate.function == entry.function &&
			    overlaps(entry.address, definition_end(entry),
				    candidate.address, definition_end(candidate)))
				return false;
			if (source_is_duplicate(entry, candidate))
				return false;
		}
	}
	return true;
}

/** Binary lookup of the logical value which owns one 16-bit address. */
[[nodiscard]] constexpr const RegisterDefinition *find_definition(
	std::span<const RegisterDefinition> entries,
	mnc::modbus::FunctionCode function, std::uint16_t address) noexcept
{
	std::size_t first = 0;
	std::size_t last = entries.size();
	while (first < last) {
		const auto middle = first + (last - first) / 2;
		const auto &candidate = entries[middle];
		const auto candidate_function =
			static_cast<std::uint8_t>(candidate.function);
		const auto wanted_function = static_cast<std::uint8_t>(function);
		if (candidate_function < wanted_function ||
		    (candidate_function == wanted_function &&
		     candidate.address <= address))
			first = middle + 1;
		else
			last = middle;
	}
	if (first == 0)
		return nullptr;
	const auto &candidate = entries[first - 1];
	return candidate.function == function && address >= candidate.address &&
	       static_cast<std::uint32_t>(address) < definition_end(candidate)
		? &candidate
		: nullptr;
}

} // namespace msap1::modbus::schema
