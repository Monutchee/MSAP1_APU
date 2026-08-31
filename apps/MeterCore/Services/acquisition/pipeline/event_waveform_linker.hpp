#pragma once

#include "msap1/meter/history/historian_ipc.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>

namespace msap1::acquisition::daemon {

/**
 * Delivers event-to-capture UUID links without ever blocking DMA ingestion.
 *
 * The historian can legitimately lag the stream record that creates the
 * catalogue row, so the worker retries the oldest link until that row exists.
 * Duplicate lifecycle updates are folded in memory and remain idempotent at
 * the database boundary.
 */
class EventWaveformLinker final {
public:
	EventWaveformLinker();
	~EventWaveformLinker();
	EventWaveformLinker(const EventWaveformLinker &) = delete;
	EventWaveformLinker &operator=(const EventWaveformLinker &) = delete;

	void enqueue(const PowerQualityEventId &event_id,
		const history::WaveformCaptureUuid &capture_uuid);
	[[nodiscard]] std::size_t pending() const;
	[[nodiscard]] std::uint64_t retry_failures() const noexcept;
	[[nodiscard]] std::uint64_t queue_overflows() const noexcept;

private:
	struct Link {
		PowerQualityEventUuid event_uuid{};
		history::WaveformCaptureUuid capture_uuid{};
		bool operator==(const Link &) const = default;
	};

	void run(std::stop_token stop);

	static constexpr std::size_t maximum_pending = 4096u;
	static constexpr std::size_t recent_capacity = 4096u;
	mutable std::mutex mutex_;
	std::condition_variable_any ready_;
	std::deque<Link> pending_;
	std::deque<Link> recent_;
	std::atomic<std::uint64_t> retry_failures_{0};
	std::atomic<std::uint64_t> queue_overflows_{0};
	std::jthread worker_;
};

} // namespace msap1::acquisition::daemon
