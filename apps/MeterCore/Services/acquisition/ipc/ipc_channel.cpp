#include "ipc/ipc_channel.hpp"

#include "support/logs.hpp"

#include <cerrno>
#include <cstring>
#include <utility>

#include <sys/resource.h>

namespace msap1::acquisition::daemon {

IpcChannel::IpcChannel(std::string socket_path)
	: socket_path_(std::move(socket_path))
{
}

IpcChannel::~IpcChannel()
{
	shutdown();
}

void IpcChannel::start()
{
	event_.open();
	server_ = std::make_unique<mnc::ipc::UnixStreamServer>(
		context_.get_executor(), socket_path_);
	server_->start(
		[this](auto connection, auto frame) {
			{
				std::scoped_lock lock(mutex_);
				requests_.push_back(
					{std::move(connection), std::move(frame)});
			}
			event_.notify();
		},
		[](const std::string &message) {
			log_message(lifecycle_log,
				mnc::logging::Priority::warning,
				"acquisition IPC connection failed: " + message,
				"ipc_connection_failed");
		});
	thread_ = std::thread([this] {
		if (::setpriority(PRIO_PROCESS, 0, 0) != 0)
			log_message(lifecycle_log, mnc::logging::Priority::warning,
				"could not demote acquisition IPC transport thread: " +
					std::string(std::strerror(errno)),
				"ipc_thread_priority_failed");
		context_.run();
	});
}

void IpcChannel::shutdown() noexcept
{
	if (server_)
		server_->stop();
	context_.stop();
	if (thread_.joinable())
		thread_.join();
}

void IpcChannel::drain(msap1::AcquisitionCommandRegistry &registry)
{
	event_.consume();
	std::deque<PendingRequest> requests;
	bool more = false;
	{
		std::scoped_lock lock(mutex_);
		constexpr std::size_t maximum_per_turn = 8u;
		for (std::size_t count = 0u;
		     count < maximum_per_turn && !requests_.empty(); ++count) {
			requests.push_back(std::move(requests_.front()));
			requests_.pop_front();
		}
		more = !requests_.empty();
	}
	for (auto &pending : requests)
		pending.connection->post_send(registry.dispatch(pending.frame));
	if (more)
		event_.notify();
}

} // namespace msap1::acquisition::daemon
