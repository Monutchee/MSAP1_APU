#include "msap1/waveform/waveform_conversion_ipc.hpp"

#include <atomic>
#include <limits>
#include <stdexcept>
#include <utility>

namespace msap1::waveform::ipc {
namespace {

constexpr std::size_t maximum_text = 1024u * 1024u;

bool valid_command(Command command) noexcept
{
	return command >= Command::capabilities && command <= Command::cancel;
}

void write_string(mnc::ipc::ByteWriter &writer, std::string_view value)
{
	if (value.size() > maximum_text)
		throw std::length_error("waveform conversion IPC text is oversized");
	writer.u32(static_cast<std::uint32_t>(value.size()));
	writer.bytes(std::as_bytes(std::span(value.data(), value.size())));
}

std::string read_string(mnc::ipc::ByteReader &reader)
{
	const auto size = reader.u32();
	if (size > maximum_text)
		throw std::invalid_argument("waveform conversion IPC text is oversized");
	const auto bytes = reader.bytes(size);
	return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}

std::uint64_t next_correlation() noexcept
{
	static std::atomic<std::uint64_t> value{1};
	return value.fetch_add(1);
}

} // namespace

mnc::ipc::Frame encode_request(const Request &request)
{
	if (!valid_command(request.command) || request.limit > maximum_chunk_bytes)
		throw std::invalid_argument("invalid waveform conversion request");
	mnc::ipc::ByteWriter writer;
	writer.u16(protocol_version);
	writer.u16(0u);
	writer.u64(request.offset);
	writer.u32(request.limit);
	writer.u32(0u);
	write_string(writer, request.owner);
	write_string(writer, request.session_id);
	write_string(writer, request.source_basename);
	write_string(writer, request.scope);
	write_string(writer, request.event_id);
	write_string(writer, request.format);
	write_string(writer, request.job_id);
	return {mnc::ipc::FrameKind::request,
		static_cast<std::uint32_t>(request.command), next_correlation(),
		writer.take()};
}

Request decode_request(const mnc::ipc::Frame &frame)
{
	if (frame.kind != mnc::ipc::FrameKind::request)
		throw std::invalid_argument("waveform conversion frame is not a request");
	Request result;
	result.command = static_cast<Command>(frame.message_type);
	if (!valid_command(result.command))
		throw std::invalid_argument("unknown waveform conversion command");
	mnc::ipc::ByteReader reader(frame.payload);
	if (reader.u16() != protocol_version)
		throw std::invalid_argument("unsupported waveform conversion IPC version");
	if (reader.u16() != 0u)
		throw std::invalid_argument("invalid waveform conversion request flags");
	result.offset = reader.u64();
	result.limit = reader.u32();
	if (reader.u32() != 0u || result.limit > maximum_chunk_bytes)
		throw std::invalid_argument("invalid waveform conversion chunk request");
	result.owner = read_string(reader);
	result.session_id = read_string(reader);
	result.source_basename = read_string(reader);
	result.scope = read_string(reader);
	result.event_id = read_string(reader);
	result.format = read_string(reader);
	result.job_id = read_string(reader);
	reader.require_finished();
	return result;
}

mnc::ipc::Frame encode_response(const Response &response,
	std::uint64_t correlation_id, Command command)
{
	mnc::ipc::ByteWriter writer;
	writer.u16(protocol_version);
	writer.u16(static_cast<std::uint16_t>(response.status));
	writer.u8(response.end_of_file ? 1u : 0u);
	writer.u8(0u);
	writer.u16(0u);
	writer.u64(response.total_size);
	write_string(writer, response.message);
	write_string(writer, response.json);
	write_string(writer, response.content);
	write_string(writer, response.filename);
	write_string(writer, response.media_type);
	write_string(writer, response.sha256);
	return {response.status == Status::ok ? mnc::ipc::FrameKind::response
		: mnc::ipc::FrameKind::error, static_cast<std::uint32_t>(command),
		correlation_id, writer.take()};
}

Response decode_response(const mnc::ipc::Frame &frame)
{
	if (frame.kind != mnc::ipc::FrameKind::response &&
	    frame.kind != mnc::ipc::FrameKind::error)
		throw std::invalid_argument("waveform conversion frame is not a response");
	mnc::ipc::ByteReader reader(frame.payload);
	if (reader.u16() != protocol_version)
		throw std::invalid_argument("unsupported waveform conversion IPC version");
	Response result;
	result.status = static_cast<Status>(reader.u16());
	result.end_of_file = reader.u8() != 0u;
	(void)reader.u8();
	(void)reader.u16();
	result.total_size = reader.u64();
	result.message = read_string(reader);
	result.json = read_string(reader);
	result.content = read_string(reader);
	result.filename = read_string(reader);
	result.media_type = read_string(reader);
	result.sha256 = read_string(reader);
	reader.require_finished();
	return result;
}

WaveformConversionClient::WaveformConversionClient(std::string path)
	: path_(std::move(path)) {}

Response WaveformConversionClient::request(Request request, int timeout_ms) const
{
	mnc::ipc::BlockingClient client(path_);
	const auto encoded = encode_request(request);
	const auto response = client.request(encoded, timeout_ms);
	if (response.correlation_id != encoded.correlation_id ||
	    response.message_type != encoded.message_type)
		throw std::runtime_error("invalid waveform conversion response identity");
	return decode_response(response);
}

} // namespace msap1::waveform::ipc
