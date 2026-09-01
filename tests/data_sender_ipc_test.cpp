#include "msap1/datalogger/data_sender_ipc.hpp"

#include <stdexcept>

namespace {

void require(bool condition, std::string_view message)
{
	if (!condition)
		throw std::runtime_error(std::string(message));
}

void request_round_trip()
{
	using namespace msap1::datalogger::ipc;
	Request request;
	request.command = Command::delete_artifacts;
	request.id = "artifact-a";
	request.ids = {"artifact-a", "artifact-b"};
	request.job_id = "job-a";
	request.state = "blocked";
	request.start_nanoseconds = 100;
	request.end_nanoseconds = 200;
	request.offset = 12;
	request.limit = 34;
	request.discard_unsent = true;
	const auto frame = encode_request(request);
	const auto decoded = decode_request(frame);
	require(decoded.command == request.command && decoded.id == request.id &&
		decoded.ids == request.ids && decoded.job_id == request.job_id &&
		decoded.state == request.state &&
		decoded.start_nanoseconds == request.start_nanoseconds &&
		decoded.end_nanoseconds == request.end_nanoseconds &&
		decoded.offset == request.offset && decoded.limit == request.limit &&
		decoded.discard_unsent,
		"Data Sender v1 request did not round-trip");
}

void response_round_trip()
{
	using namespace msap1::datalogger::ipc;
	Response response;
	response.status = Status::conflict;
	response.message = "confirmation required";
	response.json = "{\"deleted\":0}";
	response.content = "preview";
	response.filename = "file.csv";
	response.mime_type = "text/csv; charset=utf-8";
	response.sha256 = std::string(64, 'a');
	response.total_size = 1234;
	response.end_of_file = false;
	const auto frame = encode_response(response, 77,
		Command::read_artifact_chunk);
	const auto decoded = decode_response(frame);
	require(frame.kind == mnc::ipc::FrameKind::error &&
		decoded.status == response.status &&
		decoded.message == response.message &&
		decoded.json == response.json &&
		decoded.content == response.content &&
		decoded.filename == response.filename &&
		decoded.mime_type == response.mime_type &&
		decoded.sha256 == response.sha256 &&
		decoded.total_size == response.total_size &&
		!decoded.end_of_file,
		"Data Sender v1 response did not round-trip");
}

void bounds_are_enforced()
{
	using namespace msap1::datalogger::ipc;
	Request request;
	request.command = Command::retry_artifacts;
	request.ids.resize(501, "artifact");
	bool rejected = false;
	try {
		(void)encode_request(request);
	} catch (const std::invalid_argument &) {
		rejected = true;
	}
	require(rejected, "Data Sender IPC accepted an unbounded ID selection");
}

void channel_validation_round_trip()
{
	using namespace msap1::datalogger::ipc;
	Request request;
	request.command = Command::validate_channels;
	request.ids = {"channel-a", "channel-b"};
	const auto decoded = decode_request(encode_request(request));
	require(decoded.command == Command::validate_channels &&
		decoded.ids == request.ids,
		"channel-reference validation request did not round-trip");
}

} // namespace

int main()
{
	request_round_trip();
	response_round_trip();
	bounds_are_enforced();
	channel_validation_round_trip();
}
