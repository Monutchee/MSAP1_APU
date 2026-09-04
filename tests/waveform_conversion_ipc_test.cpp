#include "msap1/waveform/waveform_conversion_ipc.hpp"
#include "ipc_access_policy.hpp"

#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using namespace msap1::waveform::ipc;

void require(bool condition, std::string_view message)
{
	if (!condition)
		throw std::runtime_error(std::string(message));
}

void request_round_trip()
{
	Request source;
	source.command = Command::submit;
	source.owner = "admin";
	source.session_id = "42";
	source.source_basename = "waveform-42.mncwf";
	source.scope = "event";
	source.event_id = "12345678-1234-5234-9234-1234567890ab";
	source.format = "comtrade-zip";
	source.job_id = "unused";
	source.offset = 17;
	source.limit = 8192;
	const auto frame = encode_request(source);
	const auto decoded = decode_request(frame);
	require(decoded.command == source.command && decoded.owner == source.owner &&
		decoded.session_id == source.session_id &&
		decoded.source_basename == source.source_basename &&
		decoded.scope == source.scope && decoded.event_id == source.event_id &&
		decoded.format == source.format && decoded.job_id == source.job_id &&
		decoded.offset == source.offset && decoded.limit == source.limit,
		"request fields did not survive IPC round trip");
}

void response_round_trip()
{
	Response source;
	source.status = Status::ok;
	source.message = "ready";
	source.json = R"({"state":"ready"})";
	source.content = std::string{"A\0B", 3};
	source.filename = "waveform-42.zip";
	source.media_type = "application/zip";
	source.sha256 = std::string(64, 'a');
	source.total_size = 1234;
	source.end_of_file = false;
	const auto frame = encode_response(source, 99, Command::read_chunk);
	const auto decoded = decode_response(frame);
	require(frame.correlation_id == 99 &&
		frame.message_type == static_cast<std::uint32_t>(Command::read_chunk) &&
		decoded.status == source.status && decoded.message == source.message &&
		decoded.json == source.json && decoded.content == source.content &&
		decoded.filename == source.filename &&
		decoded.media_type == source.media_type &&
		decoded.sha256 == source.sha256 &&
		decoded.total_size == source.total_size && !decoded.end_of_file,
		"response fields or binary content did not survive IPC round trip");
}

void malformed_rejected()
{
	auto frame = encode_request(Request{});
	frame.message_type = 999;
	bool rejected = false;
	try {
		(void)decode_request(frame);
	} catch (const std::invalid_argument &) {
		rejected = true;
	}
	require(rejected, "unknown IPC command was accepted");

	Request oversized;
	oversized.limit = maximum_chunk_bytes + 1u;
	rejected = false;
	try {
		(void)encode_request(oversized);
	} catch (const std::invalid_argument &) {
		rejected = true;
	}
	require(rejected, "oversized read chunk was accepted");
}

void peer_policy_is_owner_scoped()
{
	using msap1::waveform::daemon::peer_authorized_for_uids;
	require(peer_authorized_for_uids(0u, 784u),
		"root peer was rejected");
	require(peer_authorized_for_uids(784u, 784u),
		"mnc-web peer was rejected");
	require(!peer_authorized_for_uids(785u, 784u),
		"unrelated service peer was authorized");
	require(!peer_authorized_for_uids(784u, std::nullopt),
		"missing mnc-web account accidentally authorized a peer");
}

} // namespace

int main()
{
	try {
		request_round_trip();
		response_round_trip();
		malformed_rejected();
		peer_policy_is_owner_scoped();
		std::cout << "PASS: waveform_conversion_ipc_test\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "waveform_conversion_ipc_test: " << error.what() << '\n';
		return 1;
	}
}
