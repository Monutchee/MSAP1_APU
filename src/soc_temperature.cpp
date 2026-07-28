#include "msap1/soc_temperature.hpp"

#include <charconv>
#include <fstream>
#include <stdexcept>
#include <string_view>

namespace msap1 {
namespace {

std::string read_text(const std::filesystem::path &path)
{
	std::ifstream input(path);
	if (!input)
		throw std::runtime_error("cannot read " + path.string());
	std::string value;
	std::getline(input, value);
	while (!value.empty() &&
	       (value.back() == '\r' || value.back() == '\n' ||
		value.back() == ' ' || value.back() == '\t'))
		value.pop_back();
	return value;
}

std::int64_t read_millidegrees(const std::filesystem::path &path)
{
	const auto text = read_text(path);
	std::int64_t value = 0;
	const auto parsed =
		std::from_chars(text.data(), text.data() + text.size(), value);
	if (parsed.ec != std::errc{} ||
	    parsed.ptr != text.data() + text.size())
		throw std::runtime_error("invalid temperature value in " +
			path.string());
	return value;
}

std::optional<std::size_t> sensor_index(std::string_view label)
{
	if (label == "Temp_LPD")
		return 0;
	if (label == "Temp_FPD")
		return 1;
	if (label == "Temp_PL")
		return 2;
	return std::nullopt;
}

} // namespace

std::array<SocTemperatureReading, 3>
read_soc_temperatures(const std::filesystem::path &hwmon_root)
{
	std::array<SocTemperatureReading, 3> result{{
		{"LPD", "Temp_LPD", std::nullopt},
		{"FPD", "Temp_FPD", std::nullopt},
		{"PL", "Temp_PL", std::nullopt},
	}};

	std::error_code error;
	std::filesystem::directory_iterator devices(hwmon_root, error);
	if (error)
		throw std::runtime_error("cannot inspect hardware monitors under " +
			hwmon_root.string() + ": " + error.message());

	for (const auto &device : devices) {
		std::filesystem::directory_iterator attributes(device.path(), error);
		if (error) {
			error.clear();
			continue;
		}
		for (const auto &attribute : attributes) {
			const auto filename = attribute.path().filename().string();
			if (!filename.starts_with("temp") ||
			    !filename.ends_with("_label"))
				continue;
			std::string label;
			try {
				label = read_text(attribute.path());
			} catch (const std::exception &) {
				continue;
			}
			const auto index = sensor_index(label);
			if (!index)
				continue;
			auto input_name = filename;
			input_name.replace(input_name.size() - 6, 6, "_input");
			try {
				result[*index].millidegrees_c =
					read_millidegrees(device.path() / input_name);
			} catch (const std::exception &) {
				result[*index].millidegrees_c.reset();
			}
		}
	}
	return result;
}

} // namespace msap1
