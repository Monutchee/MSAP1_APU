#pragma once

#include "mnc/service/service.hpp"
#include "msap1/waveform/waveform_conversion_ipc.hpp"

#include <boost/asio/io_context.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace msap1::waveform::daemon {

class WaveformConversionService final : public mnc::Service {
public:
	WaveformConversionService();

protected:
	void on_start() override;
	void on_reload() override;
	void on_stop() noexcept override;
	[[nodiscard]] mnc::ServiceHealth health() const override;

private:
	struct JobRecord;
	class FileSink;

	void handle(mnc::ipc::UnixStreamServer::Connection connection,
		mnc::ipc::Frame frame);
	[[nodiscard]] std::shared_ptr<JobRecord> submit(const ipc::Request &request);
	[[nodiscard]] std::shared_ptr<JobRecord> find_owned(
		std::string_view job_id, std::string_view owner);
	[[nodiscard]] ipc::Response read_chunk(const ipc::Request &request);
	[[nodiscard]] std::shared_ptr<JobRecord> cancel(const ipc::Request &request);
	void worker_loop();
	void convert(const std::shared_ptr<JobRecord> &job);
	void update_queue_positions_locked();
	void cleanup_expired_locked();
	void prepare_write_locked(const std::shared_ptr<JobRecord> &job,
		std::uint64_t current_bytes, std::size_t additional_bytes);
	void evict_locked(const std::shared_ptr<JobRecord> &job);
	void purge_artifacts();

	std::filesystem::path source_root_{"/data/mnc/waveform"};
	std::filesystem::path export_root_{"/data/mnc/waveform-exports"};
	boost::asio::io_context context_;
	mnc::ipc::UnixStreamServer server_;
	std::thread io_worker_;
	std::thread conversion_worker_;
	mutable std::mutex mutex_;
	std::condition_variable condition_;
	std::deque<std::shared_ptr<JobRecord>> queue_;
	std::unordered_map<std::string, std::shared_ptr<JobRecord>> jobs_;
	std::atomic<bool> stopping_{false};
	std::atomic<bool> worker_failed_{false};
	mutable std::mutex failure_mutex_;
	std::string failure_message_;
};

} // namespace msap1::waveform::daemon
