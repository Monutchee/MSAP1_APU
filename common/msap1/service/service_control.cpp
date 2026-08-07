#include "msap1/service/service_control.hpp"

#include <stdexcept>
#include <utility>

namespace msap1::service_control {
namespace {

constexpr std::size_t service_name_width = 64;
constexpr std::size_t unit_name_width = 96;
constexpr std::size_t state_width = 24;
constexpr std::size_t message_width = 192;
constexpr std::size_t maximum_services = 32;

bool valid_command(Command command)
{
	return command >= Command::list && command <= Command::reload;
}

} // namespace

mnc::ipc::Frame encode_request(const Request &request,
				std::uint64_t correlation)
{
	if (!valid_command(request.command))
		throw std::invalid_argument("invalid service command");
	mnc::ipc::ByteWriter writer;
	writer.u16(protocol_version);
	writer.u16(0);
	writer.fixed_string(request.service, service_name_width);
	return {mnc::ipc::FrameKind::request,
		static_cast<std::uint32_t>(request.command), correlation,
		writer.take()};
}

Request decode_request(const mnc::ipc::Frame &frame)
{
	if (frame.kind != mnc::ipc::FrameKind::request)
		throw std::invalid_argument("service frame is not a request");
	Request result{static_cast<Command>(frame.message_type), {}};
	if (!valid_command(result.command))
		throw std::invalid_argument("invalid service command");
	mnc::ipc::ByteReader reader(frame.payload);
	if (reader.u16() != protocol_version)
		throw std::invalid_argument("unsupported service protocol version");
	(void)reader.u16();
	result.service = reader.fixed_string(service_name_width);
	reader.require_finished();
	return result;
}

mnc::ipc::Frame encode_response(const Response &response,
				 std::uint64_t correlation, Command command)
{
	if (response.services.size() > maximum_services)
		throw std::length_error("too many service status entries");
	mnc::ipc::ByteWriter writer;
	writer.u16(protocol_version);
	writer.u16(static_cast<std::uint16_t>(response.status));
	writer.u32(static_cast<std::uint32_t>(response.services.size()));
	writer.fixed_string(response.message, message_width);
	for (const auto &service : response.services) {
		writer.fixed_string(service.name, service_name_width);
		writer.fixed_string(service.unit, unit_name_width);
		writer.fixed_string(service.active_state, state_width);
		writer.fixed_string(service.sub_state, state_width);
		writer.u32(service.restart_count);
		writer.u8(service.permanently_failed ? 1 : 0);
		writer.u8(0);
		writer.u16(0);
	}
	return {response.status == Status::ok ? mnc::ipc::FrameKind::response
						: mnc::ipc::FrameKind::error,
		static_cast<std::uint32_t>(command), correlation, writer.take()};
}

Response decode_response(const mnc::ipc::Frame &frame)
{
	if (frame.kind != mnc::ipc::FrameKind::response &&
	    frame.kind != mnc::ipc::FrameKind::error)
		throw std::invalid_argument("service frame is not a response");
	mnc::ipc::ByteReader reader(frame.payload);
	if (reader.u16() != protocol_version)
		throw std::invalid_argument("unsupported service protocol version");
	Response result;
	result.status = static_cast<Status>(reader.u16());
	const auto count = reader.u32();
	if (count > maximum_services)
		throw std::invalid_argument("service response count is invalid");
	result.message = reader.fixed_string(message_width);
	result.services.reserve(count);
	for (std::uint32_t index = 0; index < count; ++index) {
		mnc::ManagedServiceStatus service;
		service.name = reader.fixed_string(service_name_width);
		service.unit = reader.fixed_string(unit_name_width);
		service.active_state = reader.fixed_string(state_width);
		service.sub_state = reader.fixed_string(state_width);
		service.restart_count = reader.u32();
		service.permanently_failed = reader.u8() != 0;
		(void)reader.u8();
		(void)reader.u16();
		result.services.push_back(std::move(service));
	}
	reader.require_finished();
	return result;
}

Client::Client(std::string path) : client_(std::move(path)) {}

Response Client::request(Command command, std::string service, int timeout_ms)
{
	const auto correlation = next_correlation_++;
	auto frame = client_.request(
		encode_request({command, std::move(service)}, correlation), timeout_ms);
	if (frame.correlation_id != correlation ||
	    frame.message_type != static_cast<std::uint32_t>(command))
		throw std::runtime_error("invalid service-manager response identity");
	return decode_response(frame);
}

} // namespace msap1::service_control
