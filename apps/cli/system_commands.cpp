#include "cli.hpp"
#include "result_output.hpp"

#include "msap1/soc_temperature.hpp"

#include <algorithm>
#include <iomanip>
#include <ostream>
#include <string>
#include <vector>

namespace msap1::cli {
namespace {

struct TemperatureDto {
	std::string zone;
	std::string label;
	bool available = false;
	std::int64_t millidegrees_c = 0;
	double celsius = 0.0;
};

struct TemperatureResult {
	bool healthy = false;
	std::vector<TemperatureDto> sensors;
};

class TemperatureTextGenerator final :
	public ResultGenerator<TemperatureResult> {
public:
	int write(const TemperatureResult &result,
		  std::ostream &output) const override
	{
		output << "MSAP1 SoC temperatures\n";
		for (const auto &reading : result.sensors) {
			output << "  " << std::left << std::setw(4)
			       << reading.zone << " (" << reading.label << "): ";
			if (reading.available)
				output << std::fixed << std::setprecision(3)
				       << reading.celsius << " °C\n";
			else
				output << "unavailable\n";
		}
		return result.healthy ? 0 : 1;
	}
};

class TemperatureJsonGenerator final :
	public ResultGenerator<TemperatureResult> {
public:
	int write(const TemperatureResult &result,
		  std::ostream &output) const override
	{
		write_json_success(output, result);
		return 0;
	}
};

int show_temperatures(const Options &options, std::ostream &output)
{
	const auto readings = read_soc_temperatures();
	TemperatureResult result;
	result.healthy =
		std::ranges::all_of(readings, &SocTemperatureReading::available);
	for (const auto &reading : readings) {
		result.sensors.push_back({
			.zone = reading.zone,
			.label = reading.label,
			.available = reading.available(),
			.millidegrees_c =
				reading.millidegrees_c.value_or(0),
			.celsius = reading.available() ? reading.celsius() : 0.0,
		});
	}
	return render_result(options, result, output,
			     TemperatureTextGenerator{},
			     TemperatureJsonGenerator{});
}

} // namespace

void register_system_commands(Application &application)
{
	Command system("system", "Inspect Linux system sensors");
	system.add_subcommand(Command(
		"temperature", "Show LPD, FPD, and PL temperatures",
		show_temperatures,
		{
			.access = AccessLevel::diagnostic,
			.side_effect = SideEffect::none,
			.supports_text = true,
			.supports_json = true,
			.variants = {},
		}));
	application.add_command(std::move(system));
}

} // namespace msap1::cli
