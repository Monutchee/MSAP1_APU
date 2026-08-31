#include "pipeline/event_waveform_linker.hpp"

#include "support/logs.hpp"

#include <algorithm>
#include <chrono>
#include <string>

namespace msap1::acquisition::daemon {

using namespace std::chrono_literals;

EventWaveformLinker::EventWaveformLinker()
	: worker_([this](std::stop_token stop) { run(stop); })
{
}

EventWaveformLinker::~EventWaveformLinker()
{
	worker_.request_stop();
	ready_.notify_all();
}

void EventWaveformLinker::enqueue(const PowerQualityEventId &event_id,
	const history::WaveformCaptureUuid &capture_uuid)
{
	Link link{stable_power_quality_event_uuid(event_id), capture_uuid};
	std::scoped_lock lock(mutex_);
	if (std::ranges::find(pending_, link) != pending_.end() ||
	    std::ranges::find(recent_, link) != recent_.end())
		return;
	if (pending_.size() >= maximum_pending) {
		const auto overflows = ++queue_overflows_;
		log_message(waveform_log, mnc::logging::Priority::error,
			"event waveform link queue is full; association was not queued",
			"event_waveform_link_queue_full",
			{{"MNC_LINK_QUEUE_OVERFLOWS", std::to_string(overflows)}});
		return;
	}
	pending_.push_back(link);
	ready_.notify_one();
}

std::size_t EventWaveformLinker::pending() const
{
	std::scoped_lock lock(mutex_);
	return pending_.size();
}

std::uint64_t EventWaveformLinker::retry_failures() const noexcept
{
	return retry_failures_;
}

std::uint64_t EventWaveformLinker::queue_overflows() const noexcept
{
	return queue_overflows_;
}

void EventWaveformLinker::run(std::stop_token stop)
{
	history::ipc::HistorianClient historian;
	while (!stop.stop_requested()) {
		Link link{};
		{
			std::unique_lock lock(mutex_);
			ready_.wait(lock, stop,
				[this] { return !pending_.empty(); });
			if (stop.stop_requested())
				return;
			link = pending_.front();
		}
		try {
			historian.link_power_quality_event_waveform(
				link.event_uuid, link.capture_uuid);
			std::scoped_lock lock(mutex_);
			if (!pending_.empty() && pending_.front() == link)
				pending_.pop_front();
			recent_.push_back(link);
			if (recent_.size() > recent_capacity)
				recent_.pop_front();
		} catch (const std::exception &error) {
			const auto failures = ++retry_failures_;
			if (failures == 1u || failures % 60u == 0u)
				log_message(waveform_log,
					mnc::logging::Priority::warning,
					"event waveform link delivery is retrying: " +
						std::string(error.what()),
					"event_waveform_link_retry",
					{{"MNC_LINK_RETRY_FAILURES",
					  std::to_string(failures)}});
			std::unique_lock lock(mutex_);
			(void)ready_.wait_for(lock, stop, 1s,
				[&] { return pending_.empty() || pending_.front() != link; });
		}
	}
}

} // namespace msap1::acquisition::daemon
