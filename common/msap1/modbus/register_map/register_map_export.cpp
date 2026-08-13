#include "msap1/modbus/register_map/register_map_export.hpp"

#include "mnc/MeterDataProvider/attributes/meter_attribute.hpp"
#include "msap1/modbus/modbus_register_map.hpp"

#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <type_traits>

namespace msap1::modbus {
namespace {

using mnc::meter::MeasurementPeriod;
using mnc::modbus::FunctionCode;
using schema::MeasurementSource;
using schema::SpecialSource;

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

} // namespace

std::string export_register_map(RegisterMapFormat format)
{
	switch (format) {
	case RegisterMapFormat::text: return export_text();
	case RegisterMapFormat::csv: return export_csv();
	case RegisterMapFormat::markdown: return export_markdown();
	}
	throw std::invalid_argument("unknown Modbus map export format");
}

} // namespace msap1::modbus
