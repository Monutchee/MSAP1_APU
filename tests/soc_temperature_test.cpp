#include "msap1/soc_temperature.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

void write(const std::filesystem::path &path, const std::string &value)
{
	std::ofstream output(path);
	if (!output)
		throw std::runtime_error("failed to create test fixture");
	output << value;
}

} // namespace

int main()
{
	const auto root = std::filesystem::temp_directory_path() /
		("msap1-temperature-test-" +
		 std::to_string(std::chrono::steady_clock::now()
					.time_since_epoch()
					.count()));
	try {
		std::filesystem::create_directories(root / "hwmon7");
		std::filesystem::create_directories(root / "hwmon2");
		write(root / "hwmon7/temp1_label", "Temp_PL\n");
		write(root / "hwmon7/temp1_input", "40730\n");
		write(root / "hwmon2/temp2_label", "Temp_FPD\n");
		write(root / "hwmon2/temp2_input", "41593\n");
		write(root / "hwmon2/temp3_label", "Temp_LPD\n");
		write(root / "hwmon2/temp3_input", "41717\n");
		write(root / "hwmon2/temp4_label", "unrelated\n");
		write(root / "hwmon2/temp4_input", "99999\n");

		const auto readings = msap1::read_soc_temperatures(root);
		require(readings[0].zone == "LPD" &&
				readings[0].millidegrees_c == 41717,
			"LPD temperature was not discovered by label");
		require(readings[1].zone == "FPD" &&
				readings[1].millidegrees_c == 41593,
			"FPD temperature was not discovered by label");
		require(readings[2].zone == "PL" &&
				readings[2].millidegrees_c == 40730,
			"PL temperature was not discovered by label");

		std::filesystem::remove(root / "hwmon7/temp1_input");
		const auto partial = msap1::read_soc_temperatures(root);
		require(!partial[2].available() && partial[0].available() &&
				partial[1].available(),
			"a missing sensor did not remain independently unavailable");
		std::filesystem::remove_all(root);
		std::cout << "SoC temperature tests passed\n";
		return 0;
	} catch (const std::exception &exception) {
		std::filesystem::remove_all(root);
		std::cerr << "SoC temperature test failed: " << exception.what()
			  << '\n';
		return 1;
	}
}
