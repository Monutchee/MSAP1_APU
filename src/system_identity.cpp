#include "msap1/system_identity.hpp"

#include <array>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <unordered_map>

#include <openssl/evp.h>

namespace msap1 {
namespace {

std::string unquote(std::string value)
{
	if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
		return value.substr(1, value.size() - 2);
	return value;
}

std::unordered_map<std::string, std::string>
read_key_values(const std::filesystem::path &path)
{
	std::ifstream input(path);
	if (!input)
		return {};

	std::unordered_map<std::string, std::string> values;
	std::string line;
	while (std::getline(input, line)) {
		const auto separator = line.find('=');
		if (separator == std::string::npos || separator == 0)
			continue;
		values.insert_or_assign(
			line.substr(0, separator),
			unquote(line.substr(separator + 1)));
	}
	return values;
}

std::string value_or_empty(
	const std::unordered_map<std::string, std::string> &values,
	std::string_view key)
{
	const auto found = values.find(std::string(key));
	return found == values.end() ? std::string{} : found->second;
}

std::string md5_file(const std::filesystem::path &path)
{
	using Context = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
	Context context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
	if (!context || EVP_DigestInit_ex(context.get(), EVP_md5(), nullptr) != 1)
		throw std::runtime_error("unable to initialize MD5 digest");

	std::ifstream input(path, std::ios::binary);
	if (!input)
		throw std::runtime_error("unable to open component file");

	std::array<char, 64 * 1024> buffer{};
	while (input) {
		input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
		const auto count = input.gcount();
		if (count > 0 &&
		    EVP_DigestUpdate(context.get(), buffer.data(),
				     static_cast<std::size_t>(count)) != 1)
			throw std::runtime_error("unable to update MD5 digest");
	}
	if (!input.eof())
		throw std::runtime_error("unable to read component file");

	std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
	unsigned int length = 0;
	if (EVP_DigestFinal_ex(context.get(), digest.data(), &length) != 1)
		throw std::runtime_error("unable to finalize MD5 digest");

	std::ostringstream output;
	output << std::hex << std::setfill('0');
	for (unsigned int index = 0; index < length; ++index)
		output << std::setw(2) << static_cast<unsigned>(digest[index]);
	return output.str();
}

} // namespace

ImageIdentity read_image_identity(const std::filesystem::path &path)
{
	const auto values = read_key_values(path);
	if (values.empty())
		return {};

	return {
		true,
		value_or_empty(values, "IMAGE_ROLE"),
		value_or_empty(values, "IMAGE_LABEL"),
		value_or_empty(values, "IMAGE_RECIPE"),
		value_or_empty(values, "MACHINE"),
		value_or_empty(values, "DISTRO_VERSION"),
		value_or_empty(values, "BUILD_TIME"),
		value_or_empty(values, "BUILD_HASH"),
		value_or_empty(values, "BUILD_HASH_SHORT"),
	};
}

ComponentFingerprint fingerprint_component(const ComponentDefinition &component)
{
	ComponentFingerprint result{
		component.id,
		component.label,
		component.component_type,
		component.path.string(),
		false,
		0,
		{},
	};
	std::error_code error;
	if (!std::filesystem::is_regular_file(component.path, error) || error)
		return result;

	result.size_bytes = std::filesystem::file_size(component.path, error);
	if (error) {
		result.size_bytes = 0;
		return result;
	}

	try {
		result.md5 = md5_file(component.path);
		result.available = true;
	} catch (const std::exception &) {
		result.md5.clear();
		result.size_bytes = 0;
	}
	return result;
}

std::vector<ComponentFingerprint> system_component_fingerprints()
{
	static const std::array<ComponentDefinition, 6> components{{
		{"pl-bitstream", "Programmable logic", "FPGA bitstream",
		 "/usr/lib/firmware/xilinx/msap1/msap1-dfx-firmware.bin"},
		{"rpu-r5c0", "RPU core 0", "R5 firmware",
		 "/usr/lib/firmware/xilinx/msap1-rpu/rpu/0/R5c0.elf"},
		{"rpu-r5c1", "RPU core 1", "R5 firmware",
		 "/usr/lib/firmware/xilinx/msap1-rpu/rpu/1/R5c1.elf"},
		{"apu-acquisition", "FPGA acquisition", "APU executable",
		 "/usr/bin/msap1-fpga-acquisition"},
		{"apu-web-backend", "Web backend", "APU executable",
		 "/usr/bin/msap1-web-backend"},
		{"apu-mnc", "MNC diagnostic CLI", "APU executable",
		 "/usr/bin/mnc"},
	}};

	std::vector<ComponentFingerprint> result;
	result.reserve(components.size());
	for (const auto &component : components)
		result.push_back(fingerprint_component(component));
	return result;
}

} // namespace msap1
