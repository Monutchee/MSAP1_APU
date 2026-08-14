#include "msap1/settings/settings_ipc.hpp"

#include <atomic>
#include <glaze/glaze.hpp>
#include <stdexcept>
#include <utility>

namespace msap1::settings::ipc {
namespace {

constexpr std::size_t maximum_string = 1024u * 1024u;

bool valid_command(Command command)
{
	return command >= Command::get_active &&
		command <= Command::resolve_mqtt_assets;
}

void write_string(mnc::ipc::ByteWriter &writer, std::string_view value)
{
	if (value.size() > maximum_string)
		throw std::length_error("settings IPC string is oversized");
	writer.u32(static_cast<std::uint32_t>(value.size()));
	writer.bytes(std::as_bytes(std::span(value.data(), value.size())));
}

std::string read_string(mnc::ipc::ByteReader &reader)
{
	const auto size = reader.u32();
	if (size > maximum_string)
		throw std::invalid_argument("settings IPC string is oversized");
	const auto bytes = reader.bytes(size);
	return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}

std::uint64_t next_correlation()
{
	static std::atomic<std::uint64_t> value{1u};
	return value.fetch_add(1u);
}

} // namespace

mnc::ipc::Frame encode_request(const Request &request)
{
	if (!valid_command(request.command))
		throw std::invalid_argument("invalid settings command");
	mnc::ipc::ByteWriter writer;
	writer.u16(protocol_version);
	writer.u16(0);
	writer.u8(request.confirmed ? 1u : 0u);
	writer.u8(0);
	writer.u16(0);
	write_string(writer, request.name);
	write_string(writer, request.json);
	return {mnc::ipc::FrameKind::request,
		static_cast<std::uint32_t>(request.command), next_correlation(),
		writer.take()};
}

Request decode_request(const mnc::ipc::Frame &frame)
{
	if (frame.kind != mnc::ipc::FrameKind::request)
		throw std::invalid_argument("settings frame is not a request");
	Request result;
	result.command = static_cast<Command>(frame.message_type);
	if (!valid_command(result.command))
		throw std::invalid_argument("invalid settings command");
	mnc::ipc::ByteReader reader(frame.payload);
	if (reader.u16() != protocol_version)
		throw std::invalid_argument("unsupported settings protocol version");
	(void)reader.u16();
	result.confirmed = reader.u8() != 0u;
	(void)reader.u8();
	(void)reader.u16();
	result.name = read_string(reader);
	result.json = read_string(reader);
	reader.require_finished();
	return result;
}

mnc::ipc::Frame encode_response(const Response &response,
				 std::uint64_t correlation_id, Command command)
{
	mnc::ipc::ByteWriter writer;
	writer.u16(protocol_version);
	writer.u16(static_cast<std::uint16_t>(response.status));
	write_string(writer, response.content_hash);
	write_string(writer, response.message);
	write_string(writer, response.json);
	return {response.status == Status::ok ? mnc::ipc::FrameKind::response
					       : mnc::ipc::FrameKind::error,
		static_cast<std::uint32_t>(command), correlation_id, writer.take()};
}

mnc::ipc::Frame encode_event(const Response &event, Command command)
{
	auto frame = encode_response(event, 0u, command);
	frame.kind = mnc::ipc::FrameKind::event;
	return frame;
}

Response decode_response(const mnc::ipc::Frame &frame)
{
	if (frame.kind != mnc::ipc::FrameKind::response &&
	    frame.kind != mnc::ipc::FrameKind::error &&
	    frame.kind != mnc::ipc::FrameKind::event)
		throw std::invalid_argument("settings frame is not a response");
	mnc::ipc::ByteReader reader(frame.payload);
	if (reader.u16() != protocol_version)
		throw std::invalid_argument("unsupported settings protocol version");
	Response result;
	result.status = static_cast<Status>(reader.u16());
	result.content_hash = read_string(reader);
	result.message = read_string(reader);
	result.json = read_string(reader);
	reader.require_finished();
	return result;
}

SettingsClient::SettingsClient(std::string path) : path_(std::move(path)) {}

Response SettingsClient::request(Request request, int timeout_ms) const
{
	mnc::ipc::BlockingClient client(path_);
	const auto encoded = encode_request(request);
	const auto correlation = encoded.correlation_id;
	const auto command = encoded.message_type;
	auto response = client.request(encoded, timeout_ms);
	if (response.correlation_id != correlation ||
	    response.message_type != command)
		throw std::runtime_error("invalid settings response identity");
	return decode_response(response);
}

ProductSettings SettingsClient::active(int timeout_ms) const
{
	Request active_request;
	active_request.command = Command::get_active;
	auto response = request(std::move(active_request), timeout_ms);
	if (response.status != Status::ok)
		throw std::runtime_error(response.message.empty()
			? "settings service rejected active settings request"
			: response.message);
	return SettingsCodec::decode(response.json);
}

namespace {
void require_ok(const Response &response, std::string_view operation)
{
	if (response.status != Status::ok)
		throw std::runtime_error(response.message.empty()
			? std::string(operation) + " failed" : response.message);
}
} // namespace

void SettingsClient::set_secret(std::string name, std::string value,
	int timeout_ms) const
{
	auto response = request({.command = Command::set_named_secret,
		.name = std::move(name), .json = std::move(value)}, timeout_ms);
	require_ok(response, "set secret");
}

void SettingsClient::clear_secret(std::string name, int timeout_ms) const
{
	auto response = request({.command = Command::clear_named_secret,
		.name = std::move(name)}, timeout_ms);
	require_ok(response, "clear secret");
}

bool SettingsClient::secret_present(std::string name, int timeout_ms) const
{
	auto response = request({.command = Command::get_named_secret_status,
		.name = std::move(name)}, timeout_ms);
	require_ok(response, "get secret status");
	return response.message == "present";
}

std::map<std::string, std::string> SettingsClient::runtime_mqtt_credentials(
	int timeout_ms) const
{
	auto response = request({.command = Command::resolve_mqtt_credentials},
		timeout_ms);
	require_ok(response, "resolve MQTT credentials");
	std::map<std::string, std::string> values;
	if (const auto error = glz::read_json(values, response.json))
		throw std::runtime_error("invalid MQTT credentials response");
	return values;
}

std::map<std::string, std::string> SettingsClient::runtime_mqtt_assets(
	int timeout_ms) const
{
	auto response = request({.command = Command::resolve_mqtt_assets},
		timeout_ms);
	require_ok(response, "resolve MQTT TLS assets");
	std::map<std::string, std::string> values;
	if (const auto error = glz::read_json(values, response.json))
		throw std::runtime_error("invalid MQTT TLS assets response");
	return values;
}

void SettingsClient::put_asset(std::string name, std::string contents,
	int timeout_ms) const
{
	auto response = request({.command = Command::put_asset,
		.name = std::move(name), .json = std::move(contents)}, timeout_ms);
	require_ok(response, "upload MQTT TLS asset");
}

void SettingsClient::delete_asset(std::string name, int timeout_ms) const
{
	auto response = request({.command = Command::delete_asset,
		.name = std::move(name)}, timeout_ms);
	require_ok(response, "delete MQTT TLS asset");
}

bool SettingsClient::asset_present(std::string name, int timeout_ms) const
{
	auto response = request({.command = Command::get_asset_status,
		.name = std::move(name)}, timeout_ms);
	require_ok(response, "get MQTT TLS asset status");
	return response.message == "present";
}

std::string SettingsClient::download_asset(std::string name,
	int timeout_ms) const
{
	auto response = request({.command = Command::download_asset,
		.name = std::move(name)}, timeout_ms);
	require_ok(response, "download MQTT TLS asset");
	return response.json;
}

} // namespace msap1::settings::ipc
