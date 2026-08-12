#pragma once

/**
 * @file ipc_channel.hpp
 * @brief Unix-socket command transport bridged into the acquisition loop.
 */

#include "msap1/acquisition/ipc/acquisition_ipc.hpp"
#include "mnc/ipc/ipc.hpp"
#include "support/event_signal.hpp"

#include <boost/asio/io_context.hpp>

#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace msap1::acquisition::daemon {

/**
 * @brief Owns the acquisition control socket and hands every request to the
 *        single-threaded acquisition loop.
 *
 * Socket I/O runs on a private Asio thread; completed request frames are
 * queued under a mutex and announced through an eventfd. The poll loop
 * includes event_fd() in its descriptor set and calls drain() when it
 * becomes readable, so command handlers always execute on the acquisition
 * thread and never race the DMA/RPMsg state.
 */
class IpcChannel final {
public:
	explicit IpcChannel(std::string socket_path);
	~IpcChannel();
	IpcChannel(const IpcChannel &) = delete;
	IpcChannel &operator=(const IpcChannel &) = delete;

	/** @brief Bind the socket, start accepting, and spawn the I/O thread. */
	void start();

	/** @brief Stop the server and join the I/O thread; idempotent. */
	void shutdown() noexcept;

	/** @brief Descriptor the poll loop watches for pending requests. */
	[[nodiscard]] int event_fd() const noexcept
	{
		return event_.native_handle();
	}

	/**
	 * @brief Dispatch every queued request through @p registry and send
	 *        the responses. Runs on the acquisition thread.
	 */
	void drain(msap1::AcquisitionCommandRegistry &registry);

private:
	struct PendingRequest {
		std::shared_ptr<mnc::ipc::FramedConnection> connection;
		mnc::ipc::Frame frame;
	};

	std::string socket_path_;
	boost::asio::io_context context_;
	std::unique_ptr<mnc::ipc::UnixStreamServer> server_;
	std::thread thread_;
	EventSignal event_;
	std::mutex mutex_;
	std::deque<PendingRequest> requests_;
};

} // namespace msap1::acquisition::daemon
