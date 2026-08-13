#include "msap1/modbus/register_map/register_map_export.hpp"

#include "mnc/MeterDataProvider/attributes/meter_attribute.hpp"
#include "msap1/modbus/modbus_register_map.hpp"

#include <glaze/glaze.hpp>

#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace msap1::modbus {
namespace {

using mnc::meter::MeasurementPeriod;
using mnc::modbus::FunctionCode;
using schema::MeasurementSource;
using schema::SpecialSource;

struct AddressingDto {
	std::uint32_t protocol_address_base = 0;
	std::uint16_t register_width_bits = 16;
	std::string multiword_order = "high_word_first";
};

struct RegisterDto {
	std::uint16_t function_code = 0;
	std::string function;
	std::string table;
	std::uint16_t address = 0;
	std::string address_hex;
	std::uint16_t last_address = 0;
	std::string last_address_hex;
	std::uint16_t words = 0;
	std::string data_type;
	std::optional<std::string> period;
	std::optional<std::string> period_label;
	std::string source_kind;
	std::string source;
	std::optional<std::string> attribute;
	std::optional<std::uint16_t> attribute_index;
	std::optional<std::string> special_register;
	bool compatibility_alias = false;
};

struct BlockDto {
	std::uint16_t function_code = 0;
	std::string function;
	std::string table;
	std::uint16_t base_address = 0;
	std::string base_address_hex;
	std::uint32_t size_words = 0;
	std::uint16_t last_address = 0;
	std::string last_address_hex;
	std::string name;
};

struct DocumentDto {
	std::string schema = "mnc.modbus-register-map.v1";
	std::string product = "msap1";
	std::uint16_t register_map_version = 0;
	AddressingDto addressing;
	std::vector<RegisterDto> registers;
	std::vector<BlockDto> blocks;
};

std::string_view function_name(FunctionCode function)
{
	switch (function) {
	case FunctionCode::read_holding_registers: return "FC03 holding";
	case FunctionCode::read_input_registers: return "FC04 input";
	case FunctionCode::write_single_register: return "FC06 write single";
	case FunctionCode::write_multiple_registers: return "FC16 write multiple";
	}
	return "unknown";
}

std::string_view function_key(FunctionCode function)
{
	switch (function) {
	case FunctionCode::read_holding_registers:
		return "read_holding_registers";
	case FunctionCode::read_input_registers: return "read_input_registers";
	case FunctionCode::write_single_register:
		return "write_single_register";
	case FunctionCode::write_multiple_registers:
		return "write_multiple_registers";
	}
	return "unknown";
}

std::string_view table_name(FunctionCode function)
{
	switch (function) {
	case FunctionCode::read_input_registers: return "input_registers";
	case FunctionCode::read_holding_registers:
	case FunctionCode::write_single_register:
	case FunctionCode::write_multiple_registers:
		return "holding_registers";
	}
	return "unknown";
}

std::string_view type_name(DataType type)
{
	switch (type) {
	case DataType::uint16: return "uint16";
	case DataType::uint32: return "uint32";
	case DataType::int32: return "int32";
	case DataType::float32: return "float32";
	case DataType::uint64: return "uint64";
	}
	return "unknown";
}

std::string_view period_name(MeasurementPeriod period)
{
	switch (period) {
	case MeasurementPeriod::Basic: return "10/12-cycle";
	case MeasurementPeriod::Cycles150_180: return "150/180-cycle";
	case MeasurementPeriod::Min10: return "10-minute";
	case MeasurementPeriod::Hour2: return "2-hour";
	}
	return "unknown";
}

std::string_view period_key(MeasurementPeriod period)
{
	switch (period) {
	case MeasurementPeriod::Basic: return "basic";
	case MeasurementPeriod::Cycles150_180: return "cycles_150_180";
	case MeasurementPeriod::Min10: return "minute_10";
	case MeasurementPeriod::Hour2: return "hour_2";
	}
	return "unknown";
}

std::string_view special_name(SpecialRegister field)
{
	switch (field) {
	case SpecialRegister::map_version: return "map.version";
	case SpecialRegister::word_order_marker: return "map.word_order_marker";
	case SpecialRegister::attribute_count: return "map.attribute_count";
	case SpecialRegister::quality_mask: return "measurement.quality_mask";
	case SpecialRegister::period: return "measurement.period";
	case SpecialRegister::source_sequence: return "measurement.source_sequence";
	case SpecialRegister::configuration_generation:
		return "measurement.configuration_generation";
	}
	return "unknown";
}

std::string source_name(const RegisterDefinition &definition)
{
	return std::visit(
		[](const auto &source) {
			using Source = std::decay_t<decltype(source)>;
			if constexpr (std::is_same_v<Source, MeasurementSource>) {
				auto name = std::string(mnc::meter::describe(
					source.attribute).key);
				if (source.attribute.index)
					name += "[" +
						std::to_string(*source.attribute.index) + "]";
				return name;
			} else {
				return std::string(special_name(source.field));
			}
		},
		definition.source);
}

std::string source_period(const RegisterDefinition &definition)
{
	return std::visit(
		[](const auto &source) {
			using Source = std::decay_t<decltype(source)>;
			if constexpr (std::is_same_v<Source, MeasurementSource>)
				return std::string(period_name(source.period));
			else
				return source.period
					? std::string(period_name(*source.period))
					: std::string("-");
		},
		definition.source);
}

std::optional<MeasurementPeriod> source_period_value(
	const RegisterDefinition &definition)
{
	return std::visit(
		[](const auto &source) -> std::optional<MeasurementPeriod> {
			using Source = std::decay_t<decltype(source)>;
			if constexpr (std::is_same_v<Source, MeasurementSource>)
				return source.period;
			else
				return source.period;
		},
		definition.source);
}

std::string hex_address(std::uint16_t address)
{
	std::ostringstream output;
	output << "0x" << std::hex << std::uppercase << std::setw(4)
	       << std::setfill('0') << address;
	return output.str();
}

std::string export_text()
{
	std::ostringstream output;
	output << "MSAP1 Modbus register map v" << register_map_version << '\n'
	       << "Function       Address  Words  Type     Period         Source\n"
	       << "--------------------------------------------------------------------------\n";
	for (const auto &entry : Msap1RegisterBank::definitions())
		output << std::left << std::setw(14) << function_name(entry.function)
		       << ' ' << std::setw(8) << hex_address(entry.address)
		       << ' ' << std::setw(6) << entry.words
		       << ' ' << std::setw(8) << type_name(entry.type)
		       << ' ' << std::setw(14) << source_period(entry)
		       << ' ' << source_name(entry) << '\n';
	return output.str();
}

std::string export_csv()
{
	std::ostringstream output;
	output << "function,address,words,type,period,source\n";
	for (const auto &entry : Msap1RegisterBank::definitions())
		output << '"' << function_name(entry.function) << "\",\""
		       << hex_address(entry.address) << "\"," << entry.words << ",\""
		       << type_name(entry.type) << "\",\"" << source_period(entry)
		       << "\",\"" << source_name(entry) << "\"\n";
	return output.str();
}

std::string export_markdown()
{
	std::ostringstream output;
	output << "# MSAP1 Modbus register map v" << register_map_version << "\n\n"
	       << "| Function | Address | Words | Type | Period | Source |\n"
	       << "|---|---:|---:|---|---|---|\n";
	for (const auto &entry : Msap1RegisterBank::definitions())
		output << "| " << function_name(entry.function) << " | `"
		       << hex_address(entry.address) << "` | " << entry.words << " | "
		       << type_name(entry.type) << " | " << source_period(entry)
		       << " | `" << source_name(entry) << "` |\n";

	output << "\n## Reserved address blocks\n\n"
	       << "| Function | Base | Size | Name |\n"
	       << "|---|---:|---:|---|\n";
	for (const auto &block : Msap1RegisterBank::blocks())
		output << "| " << function_name(block.function) << " | `"
		       << hex_address(block.base) << "` | `0x" << std::hex
		       << std::uppercase << block.size << std::dec << "` | `"
		       << block.name << "` |\n";
	return output.str();
}

std::string export_json()
{
	DocumentDto document;
	document.register_map_version = register_map_version;
	const auto definitions = Msap1RegisterBank::definitions();
	document.registers.reserve(definitions.size());
	for (const auto &entry : definitions) {
		const auto last_address = static_cast<std::uint16_t>(
			static_cast<std::uint32_t>(entry.address) + entry.words - 1u);
		const auto period = source_period_value(entry);
		const auto *measurement = std::get_if<MeasurementSource>(&entry.source);
		const auto *special = std::get_if<SpecialSource>(&entry.source);
		document.registers.push_back(RegisterDto{
			.function_code = static_cast<std::uint16_t>(entry.function),
			.function = std::string(function_key(entry.function)),
			.table = std::string(table_name(entry.function)),
			.address = entry.address,
			.address_hex = hex_address(entry.address),
			.last_address = last_address,
			.last_address_hex = hex_address(last_address),
			.words = entry.words,
			.data_type = std::string(type_name(entry.type)),
			.period = period
				? std::optional<std::string>(period_key(*period))
				: std::nullopt,
			.period_label = period
				? std::optional<std::string>(period_name(*period))
				: std::nullopt,
			.source_kind = measurement ? "measurement" : "special",
			.source = source_name(entry),
			.attribute = measurement
				? std::optional<std::string>(mnc::meter::describe(
					  measurement->attribute).key)
				: std::nullopt,
			.attribute_index = measurement
				? measurement->attribute.index
				: std::nullopt,
			.special_register = special
				? std::optional<std::string>(special_name(special->field))
				: std::nullopt,
			.compatibility_alias = entry.compatibility_alias,
		});
	}

	const auto blocks = Msap1RegisterBank::blocks();
	document.blocks.reserve(blocks.size());
	for (const auto &block : blocks) {
		const auto last_address = static_cast<std::uint16_t>(
			static_cast<std::uint32_t>(block.base) + block.size - 1u);
		document.blocks.push_back(BlockDto{
			.function_code = static_cast<std::uint16_t>(block.function),
			.function = std::string(function_key(block.function)),
			.table = std::string(table_name(block.function)),
			.base_address = block.base,
			.base_address_hex = hex_address(block.base),
			.size_words = block.size,
			.last_address = last_address,
			.last_address_hex = hex_address(last_address),
			.name = std::string(block.name),
		});
	}

	/* Keep nullable columns explicit so spreadsheet generators receive one
	 * stable schema for measurement-backed and special registers alike. */
	auto encoded = glz::write<glz::opts{
		.skip_null_members = false,
		.prettify = true,
	}>(document);
	if (!encoded)
		throw std::runtime_error("cannot encode Modbus register map JSON");
	return std::move(*encoded) + '\n';
}

} // namespace

std::string export_register_map(RegisterMapFormat format)
{
	switch (format) {
	case RegisterMapFormat::text: return export_text();
	case RegisterMapFormat::csv: return export_csv();
	case RegisterMapFormat::markdown: return export_markdown();
	case RegisterMapFormat::json: return export_json();
	}
	throw std::invalid_argument("unknown Modbus map export format");
}

} // namespace msap1::modbus
