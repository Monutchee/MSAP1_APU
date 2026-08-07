#include "mnc/service/service.hpp"

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <thread>

#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace mnc {
namespace {

using namespace std::chrono_literals;

sigset_t service_signals()
{
	sigset_t set;
	::sigemptyset(&set);
	::sigaddset(&set, SIGINT);
	::sigaddset(&set, SIGTERM);
	::sigaddset(&set, SIGHUP);
	return set;
}

} // namespace

Service::Service(std::string name, std::string component)
	: name_(std::move(name)), logger_(std::move(component), "lifecycle")
{
}

void Service::request_stop() noexcept { stop_requested_ = true; }

bool Service::stop_requested() const noexcept { return stop_requested_; }

bool Service::notify(std::string_view state) const noexcept
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
	const bool abstract = address.sun_path[0] == '@';
	if (abstract)
		address.sun_path[0] = '\0';
	const auto length = static_cast<socklen_t>(
		offsetof(sockaddr_un, sun_path) + name_length + (abstract ? 0 : 1));
	const auto sent = ::sendto(fd, state.data(), state.size(), MSG_NOSIGNAL,
		reinterpret_cast<const sockaddr *>(&address), length);
	::close(fd);
	return sent == static_cast<ssize_t>(state.size());
}

int Service::execute()
{
	const auto signals = service_signals();
	if (::pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) {
		logger_.write(logging::Priority::critical,
			"failed to block lifecycle signals", "signal_setup_failed");
		return 1;
	}

	bool started = false;
	int result = 0;
	try {
		on_start();
		started = true;
		const auto initial = health();
		(void)notify("READY=1\nSTATUS=" + initial.summary);
		logger_.write(logging::Priority::notice,
			name_ + " is ready", "service_ready");

		while (!stop_requested()) {
			timespec timeout{1, 0};
			const int signal = ::sigtimedwait(&signals, nullptr, &timeout);
			if (signal == SIGINT || signal == SIGTERM) {
				request_stop();
			} else if (signal == SIGHUP) {
				on_reload();
				logger_.write(logging::Priority::notice,
					name_ + " reloaded", "service_reloaded");
			} else if (signal < 0 && errno != EAGAIN && errno != EINTR) {
				throw std::runtime_error("sigtimedwait failed: " +
					std::string(std::strerror(errno)));
			}
			const auto current = health();
			if (!current.healthy) {
				result = 1;
				request_stop();
				continue;
			}
			(void)notify("WATCHDOG=1\nSTATUS=" + current.summary);
		}
	} catch (const std::exception &error) {
		result = 1;
		logger_.write(logging::Priority::critical,
			name_ + " failed: " + error.what(), "service_failed");
	} catch (...) {
		result = 1;
		logger_.write(logging::Priority::critical,
			name_ + " failed with an unknown exception", "service_failed");
	}

	(void)notify("STOPPING=1\nSTATUS=" + name_ + " is stopping");
	if (started)
		on_stop();
	logger_.write(logging::Priority::notice,
		name_ + " stopped", "service_stopped");
	return result;
}

} // namespace mnc
