#include "cli.hpp"

#include "msap1/soc_temperature.hpp"

#include <algorithm>
#include <iomanip>
#include <ostream>

namespace msap1::cli {
namespace {

int show_temperatures(const Options &, std::ostream &output)
{
	const auto readings = read_soc_temperatures();
	output << "MSAP1 SoC temperatures\n";
	for (const auto &reading : readings) {
		output << "  " << std::left << std::setw(4) << reading.zone << " ("
		       << reading.label << "): ";
		if (reading.available())
			output << std::fixed << std::setprecision(3)
			       << reading.celsius() << " °C\n";
		else
			output << "unavailable\n";
	}
	return std::ranges::all_of(readings, &SocTemperatureReading::available)
		? 0
		: 1;
}

} // namespace

void register_system_commands(Application &application)
{
	Command system("system", "Inspect Linux system sensors");
	system.add_subcommand(Command("temperature",
		"Show LPD, FPD, and PL temperatures", show_temperatures));
	application.add_command(std::move(system));
}

} // namespace msap1::cli
