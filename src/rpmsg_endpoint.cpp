#include "msap1/rpmsg_endpoint.hpp"

#include "mnc/rpmsg_chrdev.hpp"
#include "msap1/rpu_control_protocol.h"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace msap1 {
namespace {

[[noreturn]] void throw_errno(const std::string &operation)
{
	throw std::runtime_error(operation + ": " + std::strerror(errno));
}

std::string device_path_for_fd(int fd, const std::string &fallback)
{
	std::error_code error;
	const auto path = std::filesystem::read_symlink(
		"/proc/self/fd/" + std::to_string(fd), error);
	return error ? fallback : path.string();
}

} // namespace

RpmsgEndpoint::RpmsgEndpoint(std::string service, std::string device)
{
	if (device.empty()) {
		channel_ = std::make_unique<mnc::RpmsgChrdev>(service);
		device_path_ = device_path_for_fd(channel_->fd(), service);
		return;
	}

	device_path_ = std::move(device);
	data_fd_ = ::open(device_path_.c_str(), O_RDWR | O_NONBLOCK);
	if (data_fd_ < 0)
		throw_errno("open " + device_path_);
}

RpmsgEndpoint::~RpmsgEndpoint()
{
	if (data_fd_ >= 0)
		::close(data_fd_);
}

void RpmsgEndpoint::send(const std::vector<std::uint8_t> &frame) const
{
	if (frame.empty() || frame.size() > MSAP1_RPU_MAX_FRAME_SIZE)
		throw std::invalid_argument("invalid RPMsg frame size");

	if (channel_) {
		channel_->send(frame.data(), frame.size());
		return;
	}

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
	descriptor.fd = channel_ ? channel_->fd() : data_fd_;
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
		throw std::runtime_error("RPMsg endpoint disconnected: " +
					 device_path_);

	std::array<std::uint8_t, MSAP1_RPU_MAX_FRAME_SIZE> buffer{};
	const auto size = ::read(descriptor.fd, buffer.data(), buffer.size());
	if (size < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
			return {};
		throw_errno("read " + device_path_);
	}
	return {buffer.begin(), buffer.begin() + size};
}

} // namespace msap1
