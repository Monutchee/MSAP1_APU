#pragma once

/**
 * @file event_signal.hpp
 * @brief RAII eventfd used to wake the acquisition poll loop.
 */

#include "support/logs.hpp"

#include <cerrno>
#include <cstdint>

#include <sys/eventfd.h>
#include <unistd.h>

namespace msap1::acquisition::daemon {

/**
 * @brief Owns the eventfd used to hand IPC work from the Asio thread to the
 *        acquisition loop.
 *
 * The Asio thread queues a request and calls notify(); the poll loop sees
 * the descriptor become readable, calls consume(), and drains the queue.
 * This keeps every piece of acquisition state single-threaded in the poll
 * loop while the socket I/O stays asynchronous.
 */
class EventSignal final {
public:
	EventSignal() = default;
	~EventSignal()
	{
		if (fd_ >= 0)
			::close(fd_);
	}
	EventSignal(const EventSignal &) = delete;
	EventSignal &operator=(const EventSignal &) = delete;

	/** @brief Create the eventfd; idempotent. @throws on eventfd failure. */
	void open()
	{
		if (fd_ >= 0)
			return;
		fd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
		if (fd_ < 0)
			throw_errno("create acquisition IPC eventfd");
	}

	/** @brief Descriptor to include in the poll() set. */
	[[nodiscard]] int native_handle() const noexcept { return fd_; }

	/** @brief Wake the poll loop (async-signal-safe, called from Asio). */
	void notify() noexcept
	{
		const std::uint64_t value = 1;
		while (::write(fd_, &value, sizeof(value)) < 0 && errno == EINTR) {
		}
	}

	/** @brief Reset the readable state after the loop woke up. */
	void consume() noexcept
	{
		std::uint64_t value = 0;
		while (::read(fd_, &value, sizeof(value)) < 0 && errno == EINTR) {
		}
	}

private:
	int fd_ = -1;
};

} // namespace msap1::acquisition::daemon
