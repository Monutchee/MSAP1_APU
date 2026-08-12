#include "ipc/ipc_channel.hpp"

#include "support/logs.hpp"

#include <utility>

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
	thread_ = std::thread([this] { context_.run(); });
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
	{
		std::scoped_lock lock(mutex_);
		requests.swap(requests_);
	}
	for (auto &pending : requests)
		pending.connection->post_send(registry.dispatch(pending.frame));
}

} // namespace msap1::acquisition::daemon
