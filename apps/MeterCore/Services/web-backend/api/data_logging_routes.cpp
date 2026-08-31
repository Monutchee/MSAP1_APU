/** @file data_logging_routes.cpp M19 configuration and artifact APIs. */

#include "query.hpp"
#include "response.hpp"
#include "routes.hpp"

#include "msap1/settings/definition/data_logging_settings.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstring>
#include <exception>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <unordered_map>

#include <glaze/glaze.hpp>

namespace msap1::web::api {
namespace {

using settings::DataLoggingJobSettings;
using settings::DataLoggingSettings;

struct DataLoggingConfigurationDto {
	DataLoggingSettings settings;
	std::vector<DataChannelMaterialStatus> materials;
	std::uint32_t demand_window_seconds = 600;
};

struct ChannelMaterialRequestDto {
	std::string channel_id;
	std::string kind;
	std::string value;
};

struct ChannelRequestDto {
	std::string channel_id;
};

struct ArtifactMutationDto {
	std::vector<std::string> ids;
	bool all = false;
	bool confirmed = false;
	bool discard_unsent = false;
};

template<class Integer>
Integer integer_parameter(const std::unordered_map<std::string, std::string> &values,
	std::string_view name, Integer fallback)
{
	const auto found = values.find(std::string(name));
	if (found == values.end())
		return fallback;
	Integer result{};
	const auto [end, error] = std::from_chars(found->second.data(),
		found->second.data() + found->second.size(), result);
	if (error != std::errc{} || end != found->second.data() + found->second.size())
		throw std::invalid_argument("invalid query parameter: " +
			std::string(name));
	return result;
}

std::string string_parameter(
	const std::unordered_map<std::string, std::string> &values,
	std::string_view name)
{
	const auto found = values.find(std::string(name));
	return found == values.end() ? std::string{} : found->second;
}

std::unordered_map<std::string, std::string> parameters(
	const webengine::RequestContext &context,
	std::span<const std::string_view> allowed)
{
	const auto target = context.request.target();
	auto result = query_parameters(
		std::string_view(target.data(), target.size()));
	for (const auto &[name, value] : result) {
		(void)value;
		if (std::ranges::find(allowed, name) == allowed.end())
			throw std::invalid_argument("unsupported query parameter: " + name);
	}
	return result;
}

webengine::http::status sender_error_status(datalogger::ipc::Status status)
{
	switch (status) {
	case datalogger::ipc::Status::invalid_request:
		return webengine::http::status::bad_request;
	case datalogger::ipc::Status::not_found:
		return webengine::http::status::not_found;
	case datalogger::ipc::Status::conflict:
		return webengine::http::status::conflict;
	case datalogger::ipc::Status::permission_denied:
		return webengine::http::status::forbidden;
	case datalogger::ipc::Status::unavailable:
		return webengine::http::status::service_unavailable;
	case datalogger::ipc::Status::internal_error:
	case datalogger::ipc::Status::ok:
		return webengine::http::status::internal_server_error;
	}
	return webengine::http::status::internal_server_error;
}

DataLoggingConfigurationDto configuration(AppContext &app)
{
	DataLoggingConfigurationDto result;
	const auto active = settings::SettingsCodec::decode(
		app.settings.active().json);
	result.settings = active.data_logging;
	result.demand_window_seconds = active.metering.demand.window_seconds;
	result.materials.reserve(result.settings.channels.size());
	for (const auto &channel : result.settings.channels)
		result.materials.push_back(app.data_sender.materials(channel.id));
	return result;
}

bool generation_configuration_equal(const DataLoggingJobSettings &left,
	const DataLoggingJobSettings &right)
{
	return left.source_period == right.source_period &&
		left.generation_interval_seconds == right.generation_interval_seconds &&
		left.row_interval_seconds == right.row_interval_seconds &&
		left.selections == right.selections && left.format == right.format &&
		left.destination == right.destination &&
		left.channel_ids == right.channel_ids;
}

void assign_revisions(DataLoggingSettings &candidate,
	const DataLoggingSettings &current)
{
	for (auto &job : candidate.jobs) {
		const auto previous = std::ranges::find(current.jobs, job.id,
			&DataLoggingJobSettings::id);
		if (previous == current.jobs.end()) {
			job.revision = 1;
			continue;
		}
		if (generation_configuration_equal(job, *previous))
			job.revision = previous->revision;
		else {
			if (previous->revision == std::numeric_limits<std::uint64_t>::max())
				throw std::invalid_argument("data logging job revision overflow");
			job.revision = previous->revision + 1;
		}
	}
}

void require_channel(const DataLoggingSettings &configuration,
	std::string_view channel_id)
{
	if (!settings::valid_data_channel_id(channel_id) ||
	    std::ranges::find(configuration.channels, channel_id,
		&settings::DataChannelSettings::id) == configuration.channels.end())
		throw std::out_of_range("unknown data channel ID");
}

std::vector<std::string> all_artifact_ids(AppContext &app)
{
	std::vector<std::string> result;
	constexpr std::uint32_t page_size = 500;
	for (std::uint64_t offset = 0;; offset += page_size) {
		const auto page = app.data_sender.artifacts(offset, page_size);
		for (const auto &artifact : page.artifacts)
			result.push_back(artifact.id);
		if (page.artifacts.size() < page_size)
			break;
		if (result.size() > 100000)
			throw std::runtime_error(
				"generated file count exceeds the administrative limit");
	}
	return result;
}

webengine::Response mutation_error(std::string_view route,
	const std::exception &error)
{
	if (const auto *sender = dynamic_cast<const DataSenderGatewayError *>(&error))
		return error_response(sender_error_status(sender->status()), error.what());
	if (dynamic_cast<const std::invalid_argument *>(&error))
		return error_response(webengine::http::status::bad_request, error.what());
	if (dynamic_cast<const std::out_of_range *>(&error))
		return error_response(webengine::http::status::not_found, error.what());
	log_api_failure(route, error);
	return error_response(webengine::http::status::conflict, error.what());
}

} // namespace

webengine::Response get_data_logging_configuration(
	AppContext &app, const webengine::RequestContext &)
{
	try {
		return json_response(webengine::http::status::ok, configuration(app));
	} catch (const std::exception &error) {
		log_api_failure("/api/v1/data-logging/configuration", error);
		return error_response(webengine::http::status::service_unavailable,
			error.what());
	}
}

webengine::Response put_data_logging_configuration(
	AppContext &app, const webengine::RequestContext &context)
{
	try {
		DataLoggingSettings input;
		if (glz::read_json(input, context.request.body()))
			return error_response(webengine::http::status::bad_request,
				"invalid data logging configuration JSON");
		const auto active = settings::SettingsCodec::decode(
			app.settings.active().json);
		assign_revisions(input, active.data_logging);
		input.validate(active.metering.demand.window_seconds);
		(void)app.settings.update_and_save(
			[&](settings::ProductSettings &value) {
				value.data_logging = input;
			}, 120000);
		return json_response(webengine::http::status::ok, configuration(app));
	} catch (const std::exception &error) {
		return mutation_error("/api/v1/data-logging/configuration", error);
	}
}

webengine::Response get_data_logging_status(
	AppContext &app, const webengine::RequestContext &)
{
	try {
		return json_response(webengine::http::status::ok,
			app.data_sender.status());
	} catch (const std::exception &error) {
		log_api_failure("/api/v1/data-logging/status", error);
		return error_response(webengine::http::status::service_unavailable,
			error.what());
	}
}

webengine::Response get_data_logging_artifacts(
	AppContext &app, const webengine::RequestContext &context)
{
	try {
		static constexpr std::array allowed{
			std::string_view{"offset"}, std::string_view{"limit"},
			std::string_view{"job_id"}, std::string_view{"state"},
			std::string_view{"start_nanoseconds"},
			std::string_view{"end_nanoseconds"}};
		const auto values = parameters(context, allowed);
		const auto offset = integer_parameter<std::uint64_t>(values,
			"offset", 0);
		const auto limit = integer_parameter<std::uint32_t>(values,
			"limit", 100);
		if (limit == 0 || limit > 500 || offset > 100000)
			throw std::invalid_argument("artifact page is outside its bounds");
		std::optional<std::int64_t> start;
		std::optional<std::int64_t> end;
		if (values.contains("start_nanoseconds"))
			start = integer_parameter<std::int64_t>(values,
				"start_nanoseconds", 0);
		if (values.contains("end_nanoseconds"))
			end = integer_parameter<std::int64_t>(values,
				"end_nanoseconds", 0);
		if (start && end && *end <= *start)
			throw std::invalid_argument("artifact date range is invalid");
		return json_response(webengine::http::status::ok,
			app.data_sender.artifacts(offset, limit,
				string_parameter(values, "job_id"),
				string_parameter(values, "state"), start, end));
	} catch (const std::exception &error) {
		return mutation_error("/api/v1/data-logging/artifacts", error);
	}
}

webengine::Response get_data_logging_artifact(
	AppContext &app, const webengine::RequestContext &context)
{
	try {
		static constexpr std::array allowed{std::string_view{"id"}};
		const auto id = string_parameter(parameters(context, allowed), "id");
		if (id.empty())
			throw std::invalid_argument("artifact id is required");
		return json_response(webengine::http::status::ok,
			app.data_sender.artifact(id));
	} catch (const std::exception &error) {
		return mutation_error("/api/v1/data-logging/artifact", error);
	}
}

webengine::Response get_data_logging_preview(
	AppContext &app, const webengine::RequestContext &context)
{
	try {
		static constexpr std::array allowed{
			std::string_view{"id"}, std::string_view{"limit"}};
		const auto values = parameters(context, allowed);
		const auto id = string_parameter(values, "id");
		const auto limit = integer_parameter<std::uint32_t>(values,
			"limit", 16384);
		if (id.empty() || limit == 0 || limit > 65536)
			throw std::invalid_argument("artifact preview request is invalid");
		return webengine::make_response(webengine::http::status::ok,
			app.data_sender.preview(id, limit),
			"text/plain; charset=utf-8");
	} catch (const std::exception &error) {
		return mutation_error("/api/v1/data-logging/artifacts/preview", error);
	}
}

webengine::Response post_data_logging_retry(
	AppContext &app, const webengine::RequestContext &context)
{
	try {
		ArtifactMutationDto request;
		if (glz::read_json(request, context.request.body()) || request.all ||
		    request.ids.empty() || request.ids.size() > 500)
			throw std::invalid_argument(
				"retry requires between 1 and 500 artifact IDs");
		app.data_sender.retry(std::move(request.ids));
		return json_response(webengine::http::status::ok,
			app.data_sender.status());
	} catch (const std::exception &error) {
		return mutation_error("/api/v1/data-logging/artifacts/retry", error);
	}
}

webengine::Response delete_data_logging_artifacts(
	AppContext &app, const webengine::RequestContext &context)
{
	try {
		ArtifactMutationDto request;
		if (glz::read_json(request, context.request.body()))
			throw std::invalid_argument("invalid generated-file deletion JSON");
		if (!request.confirmed)
			throw std::invalid_argument("generated-file deletion requires confirmation");
		if (request.all == !request.ids.empty())
			throw std::invalid_argument(
				"select artifact IDs or all generated files, not both");
		auto ids = request.all ? all_artifact_ids(app) : std::move(request.ids);
		if (ids.empty())
			return json_response(webengine::http::status::ok,
				datalogger::ipc::DeletionResult{});
		if (ids.size() > 100000)
			throw std::invalid_argument("too many generated files selected");
		if (!request.discard_unsent) {
			for (const auto &id : ids) {
				const auto detail = app.data_sender.artifact(id);
				const auto &item = detail.artifact;
				if (!item.local_only &&
				    (item.delivery_count == 0 ||
				     item.succeeded_count != item.delivery_count))
					throw DataSenderGatewayError(
						datalogger::ipc::Status::conflict,
						"unsent generated files require discard_unsent confirmation");
			}
		}
		datalogger::ipc::DeletionResult result;
		for (std::size_t begin = 0; begin < ids.size(); begin += 500) {
			const auto end = std::min(ids.size(), begin + 500);
			std::vector<std::string> batch(ids.begin() +
				static_cast<std::ptrdiff_t>(begin), ids.begin() +
				static_cast<std::ptrdiff_t>(end));
			const auto removed = app.data_sender.erase(std::move(batch),
				request.discard_unsent);
			result.deleted += removed.deleted;
			result.discarded_deliveries += removed.discarded_deliveries;
		}
		return json_response(webengine::http::status::ok, result);
	} catch (const std::exception &error) {
		return mutation_error("/api/v1/data-logging/artifacts", error);
	}
}

webengine::Response post_data_logging_channel_test(
	AppContext &app, const webengine::RequestContext &context)
{
	try {
		ChannelRequestDto request;
		if (glz::read_json(request, context.request.body()))
			throw std::invalid_argument("invalid channel test JSON");
		const auto active = settings::SettingsCodec::decode(
			app.settings.active().json);
		require_channel(active.data_logging, request.channel_id);
		return json_response(webengine::http::status::ok,
			app.data_sender.test_channel(std::move(request.channel_id)));
	} catch (const std::exception &error) {
		return mutation_error("/api/v1/data-logging/channels/test", error);
	}
}

webengine::Response get_data_logging_materials(
	AppContext &app, const webengine::RequestContext &context)
{
	try {
		static constexpr std::array allowed{std::string_view{"channel_id"}};
		const auto channel_id = string_parameter(
			parameters(context, allowed), "channel_id");
		const auto active = settings::SettingsCodec::decode(
			app.settings.active().json);
		require_channel(active.data_logging, channel_id);
		return json_response(webengine::http::status::ok,
			app.data_sender.materials(channel_id));
	} catch (const std::exception &error) {
		return mutation_error("/api/v1/data-logging/channel-materials", error);
	}
}

webengine::Response put_data_logging_credential(
	AppContext &app, const webengine::RequestContext &context)
{
	try {
		ChannelMaterialRequestDto request;
		if (glz::read_json(request, context.request.body()))
			throw std::invalid_argument("invalid channel credential JSON");
		const auto active = settings::SettingsCodec::decode(
			app.settings.active().json);
		require_channel(active.data_logging, request.channel_id);
		app.data_sender.set_secret(request.channel_id, request.kind,
			std::move(request.value));
		return json_response(webengine::http::status::ok,
			app.data_sender.materials(request.channel_id));
	} catch (const std::exception &error) {
		return mutation_error("/api/v1/data-logging/channel-credential", error);
	}
}

webengine::Response delete_data_logging_credential(
	AppContext &app, const webengine::RequestContext &context)
{
	try {
		ChannelMaterialRequestDto request;
		if (glz::read_json(request, context.request.body()))
			throw std::invalid_argument("invalid channel credential JSON");
		const auto active = settings::SettingsCodec::decode(
			app.settings.active().json);
		require_channel(active.data_logging, request.channel_id);
		app.data_sender.clear_secret(request.channel_id, request.kind);
		return json_response(webengine::http::status::ok,
			app.data_sender.materials(request.channel_id));
	} catch (const std::exception &error) {
		return mutation_error("/api/v1/data-logging/channel-credential", error);
	}
}

webengine::Response upload_data_logging_asset(AppContext &app,
	const webengine::RequestContext &context, const webengine::FileUpload &file)
{
	try {
		static constexpr std::array allowed{
			std::string_view{"channel_id"}, std::string_view{"kind"}};
		const auto values = parameters(context, allowed);
		const auto channel_id = string_parameter(values, "channel_id");
		const auto kind = string_parameter(values, "kind");
		const auto active = settings::SettingsCodec::decode(
			app.settings.active().json);
		require_channel(active.data_logging, channel_id);
		app.data_sender.upload_asset(channel_id, kind, file.contents);
		return json_response(webengine::http::status::ok,
			app.data_sender.materials(channel_id));
	} catch (const std::exception &error) {
		return mutation_error("/api/v1/data-logging/channel-asset", error);
	}
}

webengine::Response delete_data_logging_asset(
	AppContext &app, const webengine::RequestContext &context)
{
	try {
		ChannelMaterialRequestDto request;
		if (glz::read_json(request, context.request.body()))
			throw std::invalid_argument("invalid channel asset JSON");
		const auto active = settings::SettingsCodec::decode(
			app.settings.active().json);
		require_channel(active.data_logging, request.channel_id);
		app.data_sender.delete_asset(request.channel_id, request.kind);
		return json_response(webengine::http::status::ok,
			app.data_sender.materials(request.channel_id));
	} catch (const std::exception &error) {
		return mutation_error("/api/v1/data-logging/channel-asset", error);
	}
}

webengine::HandlerResult download_data_logging_artifact(
	AppContext &app, const webengine::RequestContext &context)
{
	try {
		static constexpr std::array allowed{std::string_view{"id"}};
		const auto id = string_parameter(parameters(context, allowed), "id");
		if (id.empty())
			throw std::invalid_argument("artifact id is required");
		const auto detail = app.data_sender.artifact(id);
		if (!detail.artifact.payload_present)
			throw DataSenderGatewayError(datalogger::ipc::Status::conflict,
				"generated-file payload is no longer retained");
		return webengine::StreamingDownload{
			detail.artifact.filename, detail.artifact.mime_type,
			detail.artifact.size_bytes,
			[&gateway = app.data_sender, id](std::uint64_t offset,
				std::span<std::byte> destination) {
				const auto requested = static_cast<std::uint32_t>(
					std::min<std::size_t>(destination.size(), 512u * 1024u));
				if (requested == 0)
					return std::size_t{0};
				const auto chunk = gateway.read_chunk(id, offset, requested);
				std::memcpy(destination.data(), chunk.content.data(),
					chunk.content.size());
				return chunk.content.size();
			}};
	} catch (const DataSenderGatewayError &error) {
		return error_response(sender_error_status(error.status()), error.what());
	} catch (const std::invalid_argument &error) {
		return error_response(webengine::http::status::bad_request,
			error.what());
	} catch (const std::exception &error) {
		log_api_failure("/api/v1/data-logging/artifacts/download", error);
		return error_response(webengine::http::status::service_unavailable,
			error.what());
	}
}

} // namespace msap1::web::api
