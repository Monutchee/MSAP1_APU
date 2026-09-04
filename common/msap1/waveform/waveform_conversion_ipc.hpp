#pragma once

#include "mnc/ipc/ipc.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace msap1::waveform::ipc {

inline constexpr std::string_view socket_path =
	"/run/monutchee/waveform-converter/converter.sock";
inline constexpr std::uint16_t protocol_version = 1;
inline constexpr std::uint32_t maximum_chunk_bytes = 512u * 1024u;

enum class Command : std::uint32_t {
	capabilities = 1,
	submit,
	status,
	read_chunk,
	cancel,
};

enum class Status : std::uint16_t {
	ok = 0,
	invalid_request,
	not_found,
	conflict,
	unprocessable,
	queue_full,
	storage_full,
	unavailable,
	permission_denied,
	internal_error,
};

struct Request {
	Command command = Command::capabilities;
	std::string owner;
	std::string session_id;
	std::string source_basename;
	std::string scope;
	std::string event_id;
	std::string format;
	std::string job_id;
	std::uint64_t offset = 0;
	std::uint32_t limit = maximum_chunk_bytes;
};

struct Response {
	Status status = Status::ok;
	std::string message;
	std::string json;
	std::string content;
	std::string filename;
	std::string media_type;
	std::string sha256;
	std::uint64_t total_size = 0;
	bool end_of_file = true;
};

/** Optional JSON payload attached to a rejected converter request. */
struct ErrorDetail {
	std::string code;
	std::vector<std::string> missing_fields;
};

struct Capabilities {
	bool healthy = true;
	std::uint16_t protocol = protocol_version;
	std::uint32_t maximum_queued_jobs = 8;
	std::uint64_t maximum_output_bytes = 1024ull * 1024ull * 1024ull;
	std::uint64_t artifact_ttl_seconds = 30u * 60u;
	std::vector<std::string> formats{
		"comtrade", "comtrade-zip", "pqdif"};
	std::vector<std::string> profiles{
		"IEC 60255-24:2013 CFF/BINARY32",
		"IEC 60255-24:2013 CFG/DAT ZIP (BINARY32)",
		"IEEE 1159.3-2025 PQDIF (normative definitions 1.0.0)"};
};

struct Job {
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

[[nodiscard]] mnc::ipc::Frame encode_request(const Request &request);
[[nodiscard]] Request decode_request(const mnc::ipc::Frame &frame);
[[nodiscard]] mnc::ipc::Frame encode_response(const Response &response,
	std::uint64_t correlation_id, Command command);
[[nodiscard]] Response decode_response(const mnc::ipc::Frame &frame);

class WaveformConversionClient final {
public:
	explicit WaveformConversionClient(
		std::string path = std::string(socket_path));
	[[nodiscard]] Response request(Request request,
		int timeout_ms = 5000) const;
	[[nodiscard]] const std::string &socket() const noexcept { return path_; }

private:
	std::string path_;
};

} // namespace msap1::waveform::ipc
