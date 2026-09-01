#pragma once

#include "mnc/ipc/ipc.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace msap1::datalogger::ipc {

inline constexpr std::string_view socket_path =
	"/run/monutchee/data-sender/data-sender.sock";
inline constexpr std::uint16_t protocol_version = 1;

enum class Command : std::uint32_t {
	get_status = 1,
	list_artifacts,
	get_artifact,
	preview_artifact,
	read_artifact_chunk,
	retry_artifacts,
	delete_artifacts,
	test_channel,
	validate_channels,
};

enum class Status : std::uint16_t {
	ok = 0,
	invalid_request,
	not_found,
	conflict,
	unavailable,
	permission_denied,
	internal_error,
};

struct Request {
	Command command = Command::get_status;
	std::string id;
	std::vector<std::string> ids;
	std::string job_id;
	std::string state;
	std::optional<std::int64_t> start_nanoseconds;
	std::optional<std::int64_t> end_nanoseconds;
	std::uint64_t offset = 0;
	std::uint32_t limit = 100;
	bool discard_unsent = false;
};

struct Response {
	Status status = Status::ok;
	std::string message;
	std::string json;
	std::string content;
	std::string filename;
	std::string mime_type;
	std::string sha256;
	std::uint64_t total_size = 0;
	bool end_of_file = true;
};

struct JobStatus {
	std::string id;
	std::uint64_t revision = 0;
	bool enabled = false;
	std::optional<std::int64_t> next_start_nanoseconds;
	std::optional<std::int64_t> next_end_nanoseconds;
	std::optional<std::int64_t> last_start_nanoseconds;
	std::optional<std::int64_t> last_end_nanoseconds;
	std::int64_t last_generated_at_nanoseconds = 0;
	std::string last_error;
};

struct ChannelStatus {
	std::string id;
	std::string name;
	std::string protocol;
	bool enabled = false;
	bool ready = false;
	std::string readiness_error;
	std::string last_test_state = "never";
	std::string last_test_message;
	std::int64_t last_test_at_nanoseconds = 0;
};

struct ServiceStatus {
	std::string health = "ready";
	std::string message;
	std::uint64_t artifact_count = 0;
	std::uint64_t outbox_count = 0;
	std::uint64_t outbox_bytes = 0;
	std::uint64_t archive_count = 0;
	std::uint64_t archive_bytes = 0;
	std::uint64_t completed_metadata_count = 0;
	std::uint64_t missing_payload_count = 0;
	std::uint64_t pending_delivery_count = 0;
	std::uint64_t blocked_delivery_count = 0;
	std::optional<std::int64_t> oldest_pending_created_at_nanoseconds;
	std::uint64_t maximum_bytes = 0;
	std::uint64_t available_bytes = 0;
	std::uint64_t minimum_free_bytes = 0;
	bool generation_allowed = true;
	std::string storage_blocking_reason;
	std::vector<JobStatus> jobs;
	std::vector<ChannelStatus> channels;
};

struct ArtifactSummary {
	std::string id;
	std::string job_id;
	std::uint64_t job_revision = 0;
	std::string filename;
	std::string mime_type;
	std::string sha256;
	std::uint64_t size_bytes = 0;
	std::int64_t source_start_nanoseconds = 0;
	std::int64_t source_end_nanoseconds = 0;
	std::int64_t generated_at_nanoseconds = 0;
	std::int64_t created_at_nanoseconds = 0;
	std::string state;
	bool local_only = false;
	bool payload_present = false;
	std::uint32_t delivery_count = 0;
	std::uint32_t succeeded_count = 0;
	std::uint32_t blocked_count = 0;
	std::string recovery_error;
};

struct DeliveryDetail {
	std::string channel_id;
	std::string state;
	std::uint32_t attempt_count = 0;
	std::int64_t next_attempt_nanoseconds = 0;
	std::int64_t last_attempt_nanoseconds = 0;
	std::string remote_result;
	std::string last_error;
};

struct ArtifactDetail {
	ArtifactSummary artifact;
	std::vector<DeliveryDetail> deliveries;
};

struct ArtifactList {
	std::vector<ArtifactSummary> artifacts;
	std::uint64_t offset = 0;
	std::uint64_t returned = 0;
};

struct DeletionResult {
	std::uint64_t deleted = 0;
	std::uint64_t discarded_deliveries = 0;
};

struct ChannelTestResult {
	std::string channel_id;
	std::string state;
	std::string message;
	std::int64_t tested_at_nanoseconds = 0;
};

[[nodiscard]] mnc::ipc::Frame encode_request(const Request &request);
[[nodiscard]] Request decode_request(const mnc::ipc::Frame &frame);
[[nodiscard]] mnc::ipc::Frame encode_response(const Response &response,
	std::uint64_t correlation_id, Command command);
[[nodiscard]] Response decode_response(const mnc::ipc::Frame &frame);

class DataSenderClient final {
public:
	explicit DataSenderClient(std::string path = std::string(socket_path));
	[[nodiscard]] Response request(Request request,
		int timeout_ms = 5000) const;

private:
	std::string path_;
};

} // namespace msap1::datalogger::ipc
