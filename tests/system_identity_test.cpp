#include "msap1/system/system_identity.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {

struct TempDirectory {
	std::filesystem::path path =
		std::filesystem::temp_directory_path() /
		("msap1-system-identity-" + std::to_string(
			std::chrono::steady_clock::now()
				.time_since_epoch()
				.count()));

	TempDirectory() { std::filesystem::create_directories(path); }
	~TempDirectory()
	{
		std::error_code error;
		std::filesystem::remove_all(path, error);
	}
};

} // namespace

int main()
{
	TempDirectory temporary;
	const auto identity_path = temporary.path / "mncos-image-info";
	{
		std::ofstream output(identity_path);
		output << "IMAGE_ROLE=\"main\"\n"
		       << "IMAGE_LABEL=\"MNCOS MSAP1 MAIN SYSTEM IMAGE\"\n"
		       << "IMAGE_RECIPE=\"msap1-image\"\n"
		       << "MACHINE=\"msap1\"\n"
		       << "DISTRO_VERSION=\"0.0.1\"\n"
		       << "BUILD_TIME=\"2026-07-29 14:44:50 UTC\"\n"
		       << "BUILD_HASH=\"22bed70cce293afb\"\n"
		       << "BUILD_HASH_SHORT=\"22bed7\"\n";
	}

	const auto identity = msap1::read_image_identity(identity_path);
	assert(identity.available);
	assert(identity.image_role == "main");
	assert(identity.image_recipe == "msap1-image");
	assert(identity.distro_version == "0.0.1");
	assert(identity.build_time == "2026-07-29 14:44:50 UTC");
	assert(identity.build_hash_short == "22bed7");

	const auto payload = temporary.path / "payload.bin";
	{
		std::ofstream output(payload, std::ios::binary);
		output << "abc";
	}
	const auto fingerprint = msap1::fingerprint_component(
		{"test", "Test component", "Test", payload});
	assert(fingerprint.available);
	assert(fingerprint.size_bytes == 3);
	assert(fingerprint.md5 == "900150983cd24fb0d6963f7d28e17f72");

	const auto missing = msap1::fingerprint_component(
		{"missing", "Missing component", "Test",
		 temporary.path / "missing"});
	assert(!missing.available);
	assert(missing.md5.empty());

	std::cout << "system identity tests passed\n";
	return 0;
}
