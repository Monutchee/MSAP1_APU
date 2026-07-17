#include "systemd_notifier.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace msap1::web {

bool SystemdNotifier::send(std::string_view state) const noexcept
{
	const char *socket_name = std::getenv("NOTIFY_SOCKET");
	if (socket_name == nullptr || socket_name[0] == '\0')
		return true;

	const std::size_t name_length = std::strlen(socket_name);
	if (name_length >= sizeof(sockaddr_un::sun_path))
		return false;

	const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return false;

	sockaddr_un address{};
	address.sun_family = AF_UNIX;
	std::memcpy(address.sun_path, socket_name, name_length + 1);
	const bool abstract_socket = address.sun_path[0] == '@';
	if (abstract_socket)
		address.sun_path[0] = '\0';
	const auto address_size = static_cast<socklen_t>(
		offsetof(sockaddr_un, sun_path) + name_length +
		(abstract_socket ? 0 : 1));
	const auto sent = ::sendto(fd, state.data(), state.size(), MSG_NOSIGNAL,
				   reinterpret_cast<const sockaddr *>(&address),
				   address_size);
	::close(fd);
	return sent == static_cast<ssize_t>(state.size());
}

bool SystemdNotifier::ready(std::string_view status) const noexcept
{
	return send("READY=1\nSTATUS=" + std::string(status));
}

bool SystemdNotifier::watchdog(std::string_view status) const noexcept
{
	return send("WATCHDOG=1\nSTATUS=" + std::string(status));
}

bool SystemdNotifier::stopping(std::string_view status) const noexcept
{
	return send("STOPPING=1\nSTATUS=" + std::string(status));
}

} // namespace msap1::web
