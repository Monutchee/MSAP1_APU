#include "systemd_notifier.hpp"

#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int main()
{
	const std::string name = "@msap1-notify-test-" +
		std::to_string(static_cast<long long>(::getpid()));
	const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return 1;

	sockaddr_un address{};
	address.sun_family = AF_UNIX;
	address.sun_path[0] = '\0';
	std::memcpy(address.sun_path + 1, name.data() + 1, name.size() - 1);
	const auto size = static_cast<socklen_t>(
		offsetof(sockaddr_un, sun_path) + name.size());
	if (::bind(fd, reinterpret_cast<const sockaddr *>(&address), size) != 0) {
		std::perror("bind test notify socket");
		return 2;
	}
	if (::setenv("NOTIFY_SOCKET", name.c_str(), 1) != 0)
		return 3;

	msap1::web::SystemdNotifier notifier;
	if (!notifier.ready("test ready"))
		return 4;
	char message[128]{};
	const auto received = ::recv(fd, message, sizeof(message), 0);
	if (received <= 0)
		return 5;
	const std::string state(message, static_cast<std::size_t>(received));
	if (state != "READY=1\nSTATUS=test ready")
		return 6;

	::close(fd);
	return 0;
}
