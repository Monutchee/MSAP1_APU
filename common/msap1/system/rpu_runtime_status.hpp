#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace msap1 {

/** Linux remoteproc state for one firmware-managed processing core. */
struct RemoteProcessorStatus {
	std::string identifier;
	std::string name;
	std::string state;
	std::string firmware;
	std::string sysfs_path;
};

/** Linux RPMsg-bus binding for one discovered channel/device. */
struct RpmsgDeviceStatus {
	std::string identifier;
	std::string name;
	std::string driver;
	std::string modalias;
	std::string sysfs_path;
};

/** Coherent Linux-side inventory used by human and machine diagnostics. */
struct RpuRuntimeStatus {
	std::vector<RemoteProcessorStatus> remote_processors;
	std::vector<RpmsgDeviceStatus> rpmsg_devices;
	std::vector<std::string> rpmsg_device_nodes;
};

/**
 * Reads remoteproc and RPMsg state without opening either control channel.
 *
 * Keeping this read-only inventory outside the CLI lets future Web or MCP
 * diagnostics reuse the same interpretation. Paths are injectable so tests do
 * not depend on target-only sysfs entries.
 */
class RpuRuntimeInspector {
public:
	explicit RpuRuntimeInspector(
		std::filesystem::path remoteproc_root =
			"/sys/class/remoteproc",
		std::filesystem::path rpmsg_root = "/sys/bus/rpmsg/devices",
		std::filesystem::path device_root = "/dev");

	[[nodiscard]] RpuRuntimeStatus inspect() const;

private:
	std::filesystem::path remoteproc_root_;
	std::filesystem::path rpmsg_root_;
	std::filesystem::path device_root_;
};

} // namespace msap1
