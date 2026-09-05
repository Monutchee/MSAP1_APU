#pragma once

#include "mnc/waveform/waveform_converter.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace msap1::web {

enum class WaveformExportStatus : std::uint16_t {
	ok = 0,
	invalid_request,
	not_found,
	conflict,
	unprocessable,
	queue_full,
	storage_full,
	unavailable,
	internal_error,
};

class WaveformExportTaskError final : public std::runtime_error {
public:
	WaveformExportTaskError(WaveformExportStatus status, std::string message,
		std::string code = {}, std::vector<std::string> missing_fields = {});
	[[nodiscard]] WaveformExportStatus status() const noexcept { return status_; }
	[[nodiscard]] const std::string &code() const noexcept { return code_; }
	[[nodiscard]] const std::vector<std::string> &missing_fields() const noexcept
	{
		return missing_fields_;
	}

private:
	WaveformExportStatus status_;
	std::string code_;
	std::vector<std::string> missing_fields_;
};

struct WaveformExportCapabilities {
	bool healthy = false;
	std::uint32_t maximum_queued_jobs = 8;
	std::uint64_t maximum_output_bytes = 1024ull * 1024ull * 1024ull;
	std::uint64_t artifact_ttl_seconds = 30u * 60u;
	std::vector<std::string> formats{
		"comtrade", "comtrade-zip", "pqdif"};
	std::vector<std::string> profiles{
		"IEC 60255-24:2013 CFF/BINARY32",
		"IEC 60255-24:2013 CFG/DAT ZIP (BINARY32)",
		"IEEE 1159.3-2025 PQDIF (normative definitions 1.0.0)"};
	std::string unavailable_reason;
};

struct WaveformExportJob {
	std::string job_id;
	std::string state;
	std::string owner;
	std::string session_id;
	std::string source_basename;
	std::string scope;
	std::string event_id;
	std::string format;
	std::string profile;
	std::uint32_t queue_position = 0;
	std::uint64_t processed_frames = 0;
	std::uint64_t total_frames = 0;
	std::string filename;
	std::uint64_t bytes = 0;
	std::string sha256;
	std::string created_at;
	std::string started_at;
	std::string completed_at;
	std::string expires_at;
	std::string error_code;
	std::string error_message;
	std::vector<std::string> missing_fields;
};

struct WaveformExportChunk {
	std::string content;
	std::string filename;
	std::string media_type;
	std::string sha256;
	std::uint64_t total_size = 0;
	bool end_of_file = true;
};

struct WaveformExportTaskOptions {
	std::filesystem::path source_root{"/data/mnc/waveform"};
	std::filesystem::path export_root{"/data/mnc/waveform-exports"};
	std::uint64_t maximum_output_bytes = 1024ull * 1024ull * 1024ull;
	std::uint64_t maximum_total_bytes = 1024ull * 1024ull * 1024ull;
	std::uint64_t minimum_free_bytes = 512ull * 1024ull * 1024ull;
	std::size_t maximum_queued_jobs = 8;
	std::chrono::seconds artifact_ttl{30u * 60u};
	std::chrono::seconds stream_lease{30u};
};

/** One process-local worker owned for the lifetime of the Web backend. */
class WaveformExportTaskManager final {
public:
	static constexpr std::uint32_t maximum_chunk_bytes = 512u * 1024u;

	explicit WaveformExportTaskManager(
		WaveformExportTaskOptions options = {});
	~WaveformExportTaskManager();
	WaveformExportTaskManager(const WaveformExportTaskManager &) = delete;
	WaveformExportTaskManager &operator=(
		const WaveformExportTaskManager &) = delete;

	[[nodiscard]] WaveformExportCapabilities capabilities() const;
	[[nodiscard]] WaveformExportJob submit(std::string owner,
		std::string session_id, std::string source_basename,
		std::string scope, std::string event_id, std::string format);
	[[nodiscard]] WaveformExportJob status(std::string_view owner,
		std::string_view job_id);
	[[nodiscard]] WaveformExportJob cancel(std::string_view owner,
		std::string_view job_id);
	[[nodiscard]] WaveformExportChunk read_chunk(std::string_view owner,
		std::string_view job_id, std::uint64_t offset, std::uint32_t limit);

private:
	struct JobRecord;
	class FileSink;

	void require_available() const;
	[[nodiscard]] std::shared_ptr<JobRecord> find_owned_locked(
		std::string_view job_id, std::string_view owner);
	void signal_completion_locked(const std::shared_ptr<JobRecord> &job);
	void worker_loop(std::stop_token stop_token);
	void convert(const std::shared_ptr<JobRecord> &job);
	void update_queue_positions_locked();
	void cleanup_expired_locked();
	void prepare_write_locked(const std::shared_ptr<JobRecord> &job,
		std::uint64_t current_bytes, std::size_t additional_bytes);
	void evict_locked(const std::shared_ptr<JobRecord> &job);
	void purge_artifacts();
	void fail_manager(std::string message) noexcept;

	WaveformExportTaskOptions options_;
	std::jthread worker_;
	mutable std::mutex mutex_;
	std::condition_variable condition_;
	std::deque<std::shared_ptr<JobRecord>> queue_;
	std::unordered_map<std::string, std::shared_ptr<JobRecord>> jobs_;
	std::atomic<bool> available_{false};
	mutable std::mutex failure_mutex_;
	std::string failure_message_;
};

} // namespace msap1::web
