#include "msap1/rpmsg_endpoint.hpp"

#include "msap1/rpu_control_protocol.h"

#include <fcntl.h>
#include <linux/rpmsg.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace msap1 {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view kRpmsgBus = "/sys/bus/rpmsg";
constexpr std::string_view kRpmsgClass = "/sys/class/rpmsg";

[[noreturn]] void throw_errno(const std::string &operation)
{
	throw std::runtime_error(operation + ": " + std::strerror(errno));
}

void close_fd(int &fd)
{
	if (fd >= 0)
		::close(fd);
	fd = -1;
}

std::string trim(std::string value)
{
	while (!value.empty() &&
	       (value.back() == '\n' || value.back() == '\r' ||
		value.back() == ' ' || value.back() == '\t'))
		value.pop_back();
	return value;
}

std::string read_text(const fs::path &path)
{
	std::ifstream input(path);
	if (!input)
		throw std::runtime_error("cannot read " + path.string());
	return trim(std::string(std::istreambuf_iterator<char>(input), {}));
}

void write_text(const fs::path &path, std::string_view value,
		bool include_null = false)
{
	const int fd = ::open(path.c_str(), O_WRONLY);
	if (fd < 0)
		throw_errno("open " + path.string());
	const std::size_t length = value.size() + (include_null ? 1u : 0u);
	const auto written = ::write(fd, value.data(), length);
	const int saved_errno = errno;
	::close(fd);
	errno = saved_errno;
	if (written != static_cast<ssize_t>(length))
		throw_errno("write " + path.string());
}

std::vector<fs::directory_entry> sorted_directory(const fs::path &path)
{
	std::vector<fs::directory_entry> entries;
	for (const auto &entry : fs::directory_iterator(path))
		entries.push_back(entry);
	std::sort(entries.begin(), entries.end(), [](const auto &left,
						       const auto &right) {
		return left.path().filename() < right.path().filename();
	});
	return entries;
}

struct Channel {
	std::string device;
	std::uint32_t destination = RPMSG_ADDR_ANY;
};

Channel find_channel(const std::string &service)
{
	const fs::path devices = fs::path(kRpmsgBus) / "devices";
	if (!fs::exists(devices))
		throw std::runtime_error("RPMsg bus is unavailable; is the R5 firmware running?");

	for (const auto &entry : sorted_directory(devices)) {
		const auto device = entry.path().filename().string();
		bool matches = device.find(service) != std::string::npos;
		const auto name_path = entry.path() / "name";
		if (!matches && fs::exists(name_path))
			matches = read_text(name_path) == service;
		if (!matches)
			continue;

		Channel result{device, RPMSG_ADDR_ANY};
		const auto dot = device.rfind('.');
		if (dot != std::string::npos) {
			try {
				const auto address = std::stoul(device.substr(dot + 1),
							       nullptr, 0);
				if (address <= std::numeric_limits<std::uint32_t>::max())
					result.destination = static_cast<std::uint32_t>(address);
			} catch (const std::exception &) {
				// The endpoint can still use RPMSG_ADDR_ANY.
			}
		}
		return result;
	}
	throw std::runtime_error("no RPMsg channel advertises service '" +
				 service + "'");
}

void bind_character_driver(const std::string &device)
{
	const fs::path device_path = fs::path(kRpmsgBus) / "devices" / device;
	if (fs::exists(device_path / "rpmsg"))
		return;

	const auto override_path = device_path / "driver_override";
	const auto current_driver = read_text(override_path);
	if (current_driver != "rpmsg_chrdev") {
		if (!current_driver.empty() && current_driver != "(null)")
			throw std::runtime_error(device + " is bound to " + current_driver);
		write_text(override_path, "rpmsg_chrdev", true);
	}

	try {
		write_text(fs::path(kRpmsgBus) / "drivers" / "rpmsg_chrdev" /
				   "bind",
			   device, true);
	} catch (const std::runtime_error &) {
		if (!fs::exists(device_path / "rpmsg"))
			throw;
	}
}

std::string find_named_data_device(const fs::path &root,
				   const std::string &service)
{
	if (!fs::exists(root))
		return {};
	for (const auto &entry : sorted_directory(root)) {
		const auto device = entry.path().filename().string();
		if (device.rfind("rpmsg", 0) != 0 ||
		    device.rfind("rpmsg_ctrl", 0) == 0)
			continue;
		const auto name_path = entry.path() / "name";
		if (fs::exists(name_path) && read_text(name_path) == service)
			return "/dev/" + device;
	}
	return {};
}

std::string find_control_device(const std::string &channel_device)
{
	const auto first_dot = channel_device.find('.');
	const auto virtio = channel_device.substr(0, first_dot);
	const fs::path devices = fs::path(kRpmsgBus) / "devices";
	std::vector<fs::path> candidates;

	for (const auto &entry : sorted_directory(devices)) {
		const auto name = entry.path().filename().string();
		if (name.rfind(virtio + ".rpmsg_ctrl.", 0) == 0)
			candidates.push_back(entry.path());
	}
	candidates.push_back(devices / channel_device);

	for (const auto &candidate : candidates) {
		const auto children = candidate / "rpmsg";
		if (!fs::exists(children))
			continue;
		for (const auto &entry : sorted_directory(children)) {
			const auto name = entry.path().filename().string();
			if (name.rfind("rpmsg_ctrl", 0) == 0)
				return "/dev/" + name;
		}
	}
	return {};
}

std::string wait_for_endpoint(const std::string &control_name,
			      const std::string &service)
{
	const auto deadline = std::chrono::steady_clock::now() +
		std::chrono::seconds(2);
	const fs::path control_path = fs::path(kRpmsgClass) / control_name;
	do {
		auto device = find_named_data_device(control_path, service);
		if (!device.empty())
			return device;
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	} while (std::chrono::steady_clock::now() < deadline);
	throw std::runtime_error("RPMsg endpoint device was not created for service '" +
				 service + "'");
}

} // namespace

RpmsgEndpoint::RpmsgEndpoint(std::string service, std::string device)
{
	try {
		if (!device.empty()) {
			device_path_ = std::move(device);
			data_fd_ = ::open(device_path_.c_str(), O_RDWR | O_NONBLOCK);
			if (data_fd_ < 0)
				throw_errno("open " + device_path_);
			return;
		}

		const auto channel = find_channel(service);
		bind_character_driver(channel.device);
		const fs::path channel_rpmsg = fs::path(kRpmsgBus) / "devices" /
			channel.device / "rpmsg";
		device_path_ = find_named_data_device(channel_rpmsg, service);

		if (device_path_.empty()) {
			const auto control_path = find_control_device(channel.device);
			if (control_path.empty())
				throw std::runtime_error("no rpmsg_ctrl device for " +
							 channel.device);
			control_fd_ = ::open(control_path.c_str(), O_RDWR | O_NONBLOCK);
			if (control_fd_ < 0)
				throw_errno("open " + control_path);

			rpmsg_endpoint_info info{};
			if (service.size() >= sizeof(info.name))
				throw std::runtime_error("RPMsg service name is too long");
			std::memcpy(info.name, service.c_str(), service.size() + 1);
			info.src = RPMSG_ADDR_ANY;
			info.dst = channel.destination;
			if (::ioctl(control_fd_, RPMSG_CREATE_EPT_IOCTL, &info) < 0)
				throw_errno("create RPMsg endpoint");
			created_endpoint_ = true;

			const auto control_name = fs::path(control_path).filename().string();
			device_path_ = wait_for_endpoint(control_name, service);
		}

		data_fd_ = ::open(device_path_.c_str(), O_RDWR | O_NONBLOCK);
		if (data_fd_ < 0)
			throw_errno("open " + device_path_);
	} catch (...) {
		close_fd(data_fd_);
		close_fd(control_fd_);
		throw;
	}
}

RpmsgEndpoint::~RpmsgEndpoint()
{
	if (created_endpoint_ && data_fd_ >= 0)
		(void)::ioctl(data_fd_, RPMSG_DESTROY_EPT_IOCTL);
	close_fd(data_fd_);
	close_fd(control_fd_);
}

void RpmsgEndpoint::send(const std::vector<std::uint8_t> &frame) const
{
	if (frame.empty() || frame.size() > MSAP1_RPU_MAX_FRAME_SIZE)
		throw std::invalid_argument("invalid RPMsg frame size");
	const auto written = ::write(data_fd_, frame.data(), frame.size());
	if (written < 0)
		throw_errno("write " + device_path_);
	if (written != static_cast<ssize_t>(frame.size()))
		throw std::runtime_error("short RPMsg write to " + device_path_);
}

std::vector<std::uint8_t>
RpmsgEndpoint::receive(std::chrono::milliseconds timeout) const
{
	pollfd descriptor{};
	descriptor.fd = data_fd_;
	descriptor.events = POLLIN;
	const auto result = ::poll(&descriptor, 1, static_cast<int>(timeout.count()));
	if (result < 0) {
		if (errno == EINTR)
			return {};
		throw_errno("poll " + device_path_);
	}
	if (result == 0)
		return {};
	if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
		throw std::runtime_error("RPMsg endpoint disconnected: " + device_path_);

	std::array<std::uint8_t, MSAP1_RPU_MAX_FRAME_SIZE> buffer{};
	const auto size = ::read(data_fd_, buffer.data(), buffer.size());
	if (size < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
			return {};
		throw_errno("read " + device_path_);
	}
	return {buffer.begin(), buffer.begin() + size};
}

} // namespace msap1
