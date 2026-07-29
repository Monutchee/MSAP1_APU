#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace msap1 {

struct ImageIdentity {
	bool available = false;
	std::string image_role;
	std::string image_label;
	std::string image_recipe;
	std::string machine;
	std::string distro_version;
	std::string build_time;
	std::string build_hash;
	std::string build_hash_short;
};

struct ComponentDefinition {
	std::string id;
	std::string label;
	std::string component_type;
	std::filesystem::path path;
};

struct ComponentFingerprint {
	std::string id;
	std::string label;
	std::string component_type;
	std::string path;
	bool available = false;
	std::uintmax_t size_bytes = 0;
	std::string md5;
};

ImageIdentity read_image_identity(
	const std::filesystem::path &path = "/etc/mncos-image-info");

ComponentFingerprint fingerprint_component(const ComponentDefinition &component);

std::vector<ComponentFingerprint> system_component_fingerprints();

} // namespace msap1
