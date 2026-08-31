#include "msap1/datalogger/data_sender_ipc.hpp"

#include <atomic>
#include <stdexcept>
#include <utility>

namespace msap1::datalogger::ipc {
namespace {

constexpr std::size_t maximum_text = 1024u * 1024u;
constexpr std::size_t maximum_ids = 500;

bool valid_command(Command command)
{
	return command >= Command::get_status &&
		command <= Command::validate_channels;
}

void write_string(mnc::ipc::ByteWriter &writer, std::string_view value)
{
	if (value.size() > maximum_text)
		throw std::length_error("Data Sender IPC text is oversized");
	writer.u32(static_cast<std::uint32_t>(value.size()));
	writer.bytes(std::as_bytes(std::span(value.data(), value.size())));
}

std::string read_string(mnc::ipc::ByteReader &reader)
{
	const auto size = reader.u32();
	if (size > maximum_text)
		throw std::invalid_argument("Data Sender IPC text is oversized");
	const auto value = reader.bytes(size);
	return {reinterpret_cast<const char *>(value.data()), value.size()};
}

std::uint64_t next_correlation()
{
	static std::atomic<std::uint64_t> value{1};
	return value.fetch_add(1);
}

} // namespace

mnc::ipc::Frame encode_request(const Request &request)
{
	if (!valid_command(request.command) || request.ids.size() > maximum_ids)
		throw std::invalid_argument("invalid Data Sender request");
	mnc::ipc::ByteWriter writer;
	writer.u16(protocol_version);
	std::uint16_t flags = request.discard_unsent ? 1u : 0u;
	flags |= request.start_nanoseconds ? 1u << 1u : 0u;
	flags |= request.end_nanoseconds ? 1u << 2u : 0u;
	writer.u16(flags);
	writer.u64(request.offset);
	writer.u32(request.limit);
	writer.i64(request.start_nanoseconds.value_or(0));
	writer.i64(request.end_nanoseconds.value_or(0));
	write_string(writer, request.id);
	write_string(writer, request.job_id);
	write_string(writer, request.state);
	writer.u32(static_cast<std::uint32_t>(request.ids.size()));
	for (const auto &id : request.ids)
		write_string(writer, id);
	return {mnc::ipc::FrameKind::request,
		static_cast<std::uint32_t>(request.command), next_correlation(),
		writer.take()};
}

Request decode_request(const mnc::ipc::Frame &frame)
{
	if (frame.kind != mnc::ipc::FrameKind::request)
		throw std::invalid_argument("Data Sender frame is not a request");
	Request result;
	result.command = static_cast<Command>(frame.message_type);
	if (!valid_command(result.command))
		throw std::invalid_argument("unknown Data Sender command");
	mnc::ipc::ByteReader reader(frame.payload);
	if (reader.u16() != protocol_version)
		throw std::invalid_argument("unsupported Data Sender IPC version");
	const auto flags = reader.u16();
	if ((flags & ~0x7u) != 0)
		throw std::invalid_argument("invalid Data Sender request flags");
	result.discard_unsent = (flags & 1u) != 0;
	result.offset = reader.u64();
	result.limit = reader.u32();
	const auto start = reader.i64();
	const auto end = reader.i64();
	if ((flags & (1u << 1u)) != 0)
		result.start_nanoseconds = start;
	if ((flags & (1u << 2u)) != 0)
		result.end_nanoseconds = end;
	result.id = read_string(reader);
	result.job_id = read_string(reader);
	result.state = read_string(reader);
	const auto count = reader.u32();
	if (count > maximum_ids)
		throw std::invalid_argument("too many Data Sender artifact IDs");
	result.ids.reserve(count);
	for (std::uint32_t index = 0; index < count; ++index)
		result.ids.push_back(read_string(reader));
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
	writer.u8(0);
	writer.u16(0);
	writer.u64(response.total_size);
	write_string(writer, response.message);
	write_string(writer, response.json);
	write_string(writer, response.content);
	write_string(writer, response.filename);
	write_string(writer, response.mime_type);
	write_string(writer, response.sha256);
	return {response.status == Status::ok ? mnc::ipc::FrameKind::response
		: mnc::ipc::FrameKind::error, static_cast<std::uint32_t>(command),
		correlation_id, writer.take()};
}

Response decode_response(const mnc::ipc::Frame &frame)
{
	if (frame.kind != mnc::ipc::FrameKind::response &&
	    frame.kind != mnc::ipc::FrameKind::error)
		throw std::invalid_argument("Data Sender frame is not a response");
	mnc::ipc::ByteReader reader(frame.payload);
	if (reader.u16() != protocol_version)
		throw std::invalid_argument("unsupported Data Sender IPC version");
	Response result;
	result.status = static_cast<Status>(reader.u16());
	result.end_of_file = reader.u8() != 0;
	(void)reader.u8();
	(void)reader.u16();
	result.total_size = reader.u64();
	result.message = read_string(reader);
	result.json = read_string(reader);
	result.content = read_string(reader);
	result.filename = read_string(reader);
	result.mime_type = read_string(reader);
	result.sha256 = read_string(reader);
	reader.require_finished();
	return result;
}

DataSenderClient::DataSenderClient(std::string path) : path_(std::move(path)) {}

Response DataSenderClient::request(Request request, int timeout_ms) const
{
	mnc::ipc::BlockingClient client(path_);
	const auto encoded = encode_request(request);
	const auto response = client.request(encoded, timeout_ms);
	if (response.correlation_id != encoded.correlation_id ||
	    response.message_type != encoded.message_type)
		throw std::runtime_error("invalid Data Sender response identity");
	return decode_response(response);
}

} // namespace msap1::datalogger::ipc
