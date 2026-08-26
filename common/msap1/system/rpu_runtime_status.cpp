#include "msap1/system/rpu_runtime_status.hpp"

#include <algorithm>
#include <fstream>
#include <system_error>
#include <utility>

namespace msap1 {
namespace {

std::string read_first_line(const std::filesystem::path &path)
{
	std::ifstream input(path);
	std::string value;
	if (input)
		std::getline(input, value);
	return value;
}

std::string linked_filename(const std::filesystem::path &path)
{
	std::error_code error;
	const auto target = std::filesystem::read_symlink(path, error);
	return error ? std::string{} : target.filename().string();
}

std::vector<std::filesystem::directory_entry>
directory_entries(const std::filesystem::path &root)
{
	std::vector<std::filesystem::directory_entry> entries;
	std::error_code error;
	for (std::filesystem::directory_iterator iterator(root, error), end;
	     !error && iterator != end; iterator.increment(error))
		entries.push_back(*iterator);
	std::ranges::sort(entries, {}, [](const auto &entry) {
		return entry.path().filename().string();
	});
	return entries;
}

} // namespace

RpuRuntimeInspector::RpuRuntimeInspector(
	std::filesystem::path remoteproc_root,
	std::filesystem::path rpmsg_root,
	std::filesystem::path device_root)
	: remoteproc_root_(std::move(remoteproc_root)),
	  rpmsg_root_(std::move(rpmsg_root)),
	  device_root_(std::move(device_root))
{
}

RpuRuntimeStatus RpuRuntimeInspector::inspect() const
{
	RpuRuntimeStatus result;
	for (const auto &entry : directory_entries(remoteproc_root_)) {
		const auto path = entry.path();
		result.remote_processors.push_back({
			.identifier = path.filename().string(),
			.name = read_first_line(path / "name"),
			.state = read_first_line(path / "state"),
			.firmware = read_first_line(path / "firmware"),
			.sysfs_path = path.string(),
		});
	}

	for (const auto &entry : directory_entries(rpmsg_root_)) {
		const auto path = entry.path();
		result.rpmsg_devices.push_back({
			.identifier = path.filename().string(),
			.name = read_first_line(path / "name"),
			.driver = linked_filename(path / "driver"),
			.modalias = read_first_line(path / "modalias"),
			.sysfs_path = path.string(),
		});
	}

	for (const auto &entry : directory_entries(device_root_)) {
		const auto name = entry.path().filename().string();
		if (name.starts_with("rpmsg"))
			result.rpmsg_device_nodes.push_back(entry.path().string());
	}
	return result;
}

} // namespace msap1
