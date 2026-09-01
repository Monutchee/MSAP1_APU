#include "gateway/data_sender_gateway.hpp"

#include "msap1/settings/definition/data_logging_settings.hpp"

#include <algorithm>
#include <array>
#include <utility>

#include <glaze/glaze.hpp>

namespace msap1::web {
namespace {

template<class T>
T decode(std::string_view json, std::string_view description)
{
	T result;
	if (const auto error = glz::read_json(result, json))
		throw std::runtime_error("invalid Data Sender " +
			std::string(description) + " response");
	return result;
}

} // namespace

DataSenderGatewayError::DataSenderGatewayError(
	datalogger::ipc::Status status, std::string message)
	: std::runtime_error(message.empty() ? "Data Sender request failed" : message),
	  status_(status)
{
}

datalogger::ipc::Status DataSenderGatewayError::status() const noexcept
{
	return status_;
}

datalogger::ipc::Response DataSenderGateway::require_ok(
	datalogger::ipc::Request request, int timeout_ms) const
{
	auto response = sender_.request(std::move(request), timeout_ms);
	if (response.status != datalogger::ipc::Status::ok)
		throw DataSenderGatewayError(response.status,
			std::move(response.message));
	return response;
}

DataSenderGateway::ServiceStatus DataSenderGateway::status(
	int timeout_ms) const
{
	datalogger::ipc::Request request;
	request.command = datalogger::ipc::Command::get_status;
	return decode<ServiceStatus>(require_ok(std::move(request), timeout_ms).json,
		"status");
}

DataSenderGateway::ArtifactList DataSenderGateway::artifacts(
	std::uint64_t offset, std::uint32_t limit, std::string job_id,
	std::string state, std::optional<std::int64_t> start,
	std::optional<std::int64_t> end, int timeout_ms) const
{
	datalogger::ipc::Request request;
	request.command = datalogger::ipc::Command::list_artifacts;
	request.offset = offset;
	request.limit = limit;
	request.job_id = std::move(job_id);
	request.state = std::move(state);
	request.start_nanoseconds = start;
	request.end_nanoseconds = end;
	return decode<ArtifactList>(require_ok(std::move(request), timeout_ms).json,
		"artifact list");
}

DataSenderGateway::ArtifactDetail DataSenderGateway::artifact(
	std::string id, int timeout_ms) const
{
	datalogger::ipc::Request request;
	request.command = datalogger::ipc::Command::get_artifact;
	request.id = std::move(id);
	return decode<ArtifactDetail>(require_ok(std::move(request), timeout_ms).json,
		"artifact detail");
}

std::string DataSenderGateway::preview(std::string id, std::uint32_t limit,
	int timeout_ms) const
{
	datalogger::ipc::Request request;
	request.command = datalogger::ipc::Command::preview_artifact;
	request.id = std::move(id);
	request.limit = limit;
	return require_ok(std::move(request), timeout_ms).content;
}

DataSenderGateway::Chunk DataSenderGateway::read_chunk(
	std::string id, std::uint64_t offset, std::uint32_t limit,
	int timeout_ms) const
{
	datalogger::ipc::Request request;
	request.command = datalogger::ipc::Command::read_artifact_chunk;
	request.id = std::move(id);
	request.offset = offset;
	request.limit = limit;
	return require_ok(std::move(request), timeout_ms);
}

void DataSenderGateway::retry(std::vector<std::string> ids,
	int timeout_ms) const
{
	datalogger::ipc::Request request;
	request.command = datalogger::ipc::Command::retry_artifacts;
	request.ids = std::move(ids);
	(void)require_ok(std::move(request), timeout_ms);
}

DataSenderGateway::DeletionResult DataSenderGateway::erase(
	std::vector<std::string> ids, bool discard_unsent, int timeout_ms) const
{
	datalogger::ipc::Request request;
	request.command = datalogger::ipc::Command::delete_artifacts;
	request.ids = std::move(ids);
	request.discard_unsent = discard_unsent;
	return decode<DeletionResult>(require_ok(std::move(request),
		timeout_ms).json, "deletion");
}

DataSenderGateway::ChannelTestResult DataSenderGateway::test_channel(
	std::string id, int timeout_ms) const
{
	datalogger::ipc::Request request;
	request.command = datalogger::ipc::Command::test_channel;
	request.id = std::move(id);
	return decode<ChannelTestResult>(require_ok(std::move(request),
		timeout_ms).json, "channel test");
}

std::string DataSenderGateway::scoped_name(std::string_view channel_id,
	std::string_view kind, bool asset)
{
	if (!settings::valid_data_channel_id(channel_id))
		throw std::invalid_argument("invalid data channel ID");
	static constexpr std::array secrets{
		std::string_view{"password"}, std::string_view{"bearer-token"},
		std::string_view{"private-key-passphrase"}};
	static constexpr std::array assets{
		std::string_view{"ca"}, std::string_view{"client-certificate"},
		std::string_view{"client-key"},
		std::string_view{"sftp-private-key"},
		std::string_view{"known-hosts"}};
	const auto allowed = asset
		? std::ranges::find(assets, kind) != assets.end()
		: std::ranges::find(secrets, kind) != secrets.end();
	if (!allowed)
		throw std::invalid_argument("unknown data channel material kind");
	return "data-channel." + std::string(channel_id) + "." +
		std::string(kind);
}

DataChannelMaterialStatus DataSenderGateway::materials(
	std::string_view channel_id, int timeout_ms) const
{
	const auto present = [&](std::string_view kind, bool asset) {
		const auto name = scoped_name(channel_id, kind, asset);
		return asset ? settings_.asset_present(name, timeout_ms)
			: settings_.secret_present(name, timeout_ms);
	};
	return {std::string(channel_id),
		present("password", false), present("bearer-token", false),
		present("private-key-passphrase", false), present("ca", true),
		present("client-certificate", true), present("client-key", true),
		present("sftp-private-key", true), present("known-hosts", true)};
}

void DataSenderGateway::set_secret(std::string_view channel_id,
	std::string_view kind, std::string value, int timeout_ms) const
{
	if (value.empty())
		throw std::invalid_argument("credential value must not be empty");
	settings_.set_secret(scoped_name(channel_id, kind, false),
		std::move(value), timeout_ms);
}

void DataSenderGateway::clear_secret(std::string_view channel_id,
	std::string_view kind, int timeout_ms) const
{
	settings_.clear_secret(scoped_name(channel_id, kind, false), timeout_ms);
}

void DataSenderGateway::upload_asset(std::string_view channel_id,
	std::string_view kind, std::string contents, int timeout_ms) const
{
	settings_.put_asset(scoped_name(channel_id, kind, true),
		std::move(contents), timeout_ms);
}

void DataSenderGateway::delete_asset(std::string_view channel_id,
	std::string_view kind, int timeout_ms) const
{
	settings_.delete_asset(scoped_name(channel_id, kind, true), timeout_ms);
}

} // namespace msap1::web
