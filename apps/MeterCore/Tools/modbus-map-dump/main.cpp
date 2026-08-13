#include "msap1/modbus/register_map/register_map_export.hpp"

#include <exception>
#include <iostream>
#include <string_view>

namespace {

msap1::modbus::RegisterMapFormat format(std::string_view value)
{
	if (value == "text")
		return msap1::modbus::RegisterMapFormat::text;
	if (value == "csv")
		return msap1::modbus::RegisterMapFormat::csv;
	if (value == "markdown")
		return msap1::modbus::RegisterMapFormat::markdown;
	if (value == "json")
		return msap1::modbus::RegisterMapFormat::json;
	throw std::invalid_argument(
		"format must be text, csv, markdown, or json");
}

void usage(std::ostream &output)
{
	output << "Usage: modbus-map-dump [--format text|csv|markdown|json]\n";
}

} // namespace

int main(int argc, char **argv)
{
	try {
		auto selected = msap1::modbus::RegisterMapFormat::text;
		for (int index = 1; index < argc; ++index) {
			const auto argument = std::string_view(argv[index]);
			if (argument == "--help") {
				usage(std::cout);
				return 0;
			}
			if (argument != "--format" || index + 1 >= argc)
				throw std::invalid_argument("unknown or incomplete option");
			selected = format(argv[++index]);
		}
		std::cout << msap1::modbus::export_register_map(selected);
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "modbus-map-dump: " << error.what() << '\n';
		usage(std::cerr);
		return 2;
	}
}
