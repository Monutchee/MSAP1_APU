#include "msap1/acquisition_ipc.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace msap1 {
namespace {

[[noreturn]] void throw_errno(const std::string &operation)
{
	throw std::runtime_error(operation + ": " + std::strerror(errno));
}

} // namespace

AcquisitionClient::AcquisitionClient(std::string socket_path)
	: socket_path_(std::move(socket_path))
{
}

AcquisitionResponse AcquisitionClient::request(AcquisitionCommand command,
						int timeout_ms) const
{
	const int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (fd < 0)
		throw_errno("create acquisition socket");

	auto close_fd = [&]() { ::close(fd); };
	sockaddr_un address{};
	address.sun_family = AF_UNIX;
	if (socket_path_.size() >= sizeof(address.sun_path)) {
		close_fd();
		throw std::invalid_argument("acquisition socket path is too long");
	}
	std::memcpy(address.sun_path, socket_path_.c_str(), socket_path_.size() + 1);
	if (::connect(fd, reinterpret_cast<sockaddr *>(&address),
		      sizeof(address)) < 0) {
		const int saved_errno = errno;
		close_fd();
		errno = saved_errno;
		throw_errno("connect " + socket_path_);
	}

	AcquisitionRequest request{};
	request.command = command;
	request.sequence = static_cast<std::uint64_t>(
		std::chrono::steady_clock::now().time_since_epoch().count());
	if (::send(fd, &request, sizeof(request), MSG_NOSIGNAL) !=
	    static_cast<ssize_t>(sizeof(request))) {
		const int saved_errno = errno;
		close_fd();
		errno = saved_errno;
		throw_errno("send acquisition request");
	}

	pollfd descriptor{fd, POLLIN, 0};
	const int poll_result = ::poll(&descriptor, 1, timeout_ms);
	if (poll_result <= 0) {
		const int saved_errno = errno;
		close_fd();
		if (poll_result == 0)
			throw std::runtime_error("timed out waiting for acquisition daemon");
		errno = saved_errno;
		throw_errno("poll acquisition socket");
	}

	AcquisitionResponse response{};
	const auto size = ::recv(fd, &response, sizeof(response), 0);
	close_fd();
	if (size != static_cast<ssize_t>(sizeof(response)))
		throw std::runtime_error("invalid acquisition daemon response length");
	if (response.magic != acquisition_ipc_magic ||
	    response.version != acquisition_ipc_version ||
	    response.sequence != request.sequence)
		throw std::runtime_error("invalid acquisition daemon response");
	return response;
}

} // namespace msap1
