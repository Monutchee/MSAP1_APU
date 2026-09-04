/**
 * @file waveform_routes.cpp
 * @brief Waveform endpoints: engine status, manual capture triggers, and
 *        session deletion.
 */

#include "health_dto.hpp"
#include "openapi.hpp"
#include "query.hpp"
#include "response.hpp"
#include "routes.hpp"

#include "msap1/waveform/mncwf_v4_export.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glaze/glaze.hpp>

namespace msap1::web::api {

namespace {

/** Body of POST /api/v1/waveforms/trigger. */
struct WaveformTriggerDto {
	std::uint32_t pretrigger_ms = msap1::waveform_duration_unspecified;
	std::uint32_t posttrigger_ms = msap1::waveform_duration_unspecified;
	/** Capture-file decimation divisor; 0 uses the persisted default. */
	std::uint32_t decimation = 0;
};

/** Body of DELETE /api/v1/waveforms. */
struct WaveformDeleteDto {
	std::uint64_t session_id = 0;
	bool all = false;
	bool confirmed = false;
};

/** One capture session in the waveform status document. */
struct WaveformSessionDto {
	std::uint64_t id;
	std::string state;
	std::uint64_t trigger_sequence;
	std::uint64_t first_sequence;
	std::uint64_t last_sequence;
	std::uint64_t trigger_tai_nanoseconds;
	std::uint64_t trigger_realtime_nanoseconds;
	std::uint32_t sample_rate_hz;
	std::uint32_t event_count;
	std::string origin;
	std::uint32_t decimation;
	std::string filename;
	std::uint64_t continuation_of_session_id;
	std::uint64_t master_session_id;
	std::string capture_uuid;
	std::uint32_t format_version;
	std::string compression;
	std::uint64_t stored_bytes;
	std::uint64_t logical_sample_bytes;
};

struct WaveformArchiveDiscoveryDto {
	std::string state;
	std::uint64_t scanned_files;
	std::uint64_t total_files;
	std::uint64_t rejected_files;
};

struct WaveformPageDto {
	std::string origin;
	std::uint32_t limit;
	std::uint64_t total_sessions;
	std::uint64_t completed_sessions;
	std::uint64_t incomplete_sessions;
	std::uint64_t active_sessions;
	std::uint64_t returned_sessions;
	std::optional<std::uint64_t> next_before_session_id;
};

/** Body of GET /api/v1/waveforms (also returned by trigger/delete). */
struct WaveformDto {
	bool running;
	bool active_session;
	std::uint32_t sample_rate_hz;
	std::uint32_t transport_ring_blocks;
	std::uint64_t blocks;
	std::uint64_t frames;
	std::uint64_t bytes;
	std::uint64_t invalid_blocks;
	std::uint64_t sequence_gaps;
	std::uint64_t transport_overrun_blocks;
	std::uint64_t materialization_failures;
	std::uint32_t pl_dropped_frames;
	std::uint64_t max_capture_frames;
	std::uint64_t history_oldest_sequence;
	std::uint64_t history_latest_sequence;
	std::uint64_t history_capacity_frames;
	std::uint64_t completed_sessions;
	std::uint64_t incomplete_sessions;
	std::uint64_t archive_limit_bytes;
	std::uint64_t archive_stored_bytes;
	std::uint64_t expired_sessions;
	std::uint64_t retention_failures;
	WaveformArchiveDiscoveryDto archive_discovery;
	WaveformPageDto page;
	std::vector<std::string> export_formats;
	std::vector<WaveformSessionDto> sessions;
};

struct WaveformSessionLookupDto {
	std::string capture_uuid;
	WaveformArchiveDiscoveryDto archive_discovery;
	std::optional<WaveformSessionDto> session;
};

struct WaveformBatchLookupRequestDto {
	std::vector<std::string> capture_uuids;
};

struct WaveformBatchLookupDto {
	WaveformArchiveDiscoveryDto archive_discovery;
	std::vector<std::optional<WaveformSessionDto>> sessions;
};

std::string waveform_state_name(msap1::WaveformSessionState state)
{
	switch (state) {
	case msap1::WaveformSessionState::capturing: return "capturing";
	case msap1::WaveformSessionState::complete: return "complete";
	case msap1::WaveformSessionState::incomplete: return "incomplete";
	}
	return "unknown";
}

std::string archive_discovery_state_name(
	msap1::WaveformArchiveDiscoveryState state)
{
	switch (state) {
	case msap1::WaveformArchiveDiscoveryState::not_started:
		return "not_started";
	case msap1::WaveformArchiveDiscoveryState::scanning:
		return "scanning";
	case msap1::WaveformArchiveDiscoveryState::complete:
		return "complete";
	case msap1::WaveformArchiveDiscoveryState::cancelled:
		return "cancelled";
	case msap1::WaveformArchiveDiscoveryState::failed:
		return "failed";
	}
	return "failed";
}

std::string waveform_origin(std::uint32_t mask)
{
	const auto manual = (mask &
		((1u << static_cast<unsigned>(WaveformTriggerSource::manual_cli)) |
		 (1u << static_cast<unsigned>(WaveformTriggerSource::manual_web)))) != 0u;
	const auto power_quality = (mask &
		(1u << static_cast<unsigned>(WaveformTriggerSource::pq_event))) != 0u;
	if (manual && power_quality) return "mixed";
	if (manual) return "manual";
	if (power_quality) return "power_quality";
	return "legacy";
}

std::string waveform_compression(WaveformCompression compression)
{
	switch (compression) {
	case WaveformCompression::none: return "none";
	case WaveformCompression::zstd_chunks: return "zstd_chunks";
	case WaveformCompression::mixed_raw_zstd_chunks:
		return "mixed_raw_zstd_chunks";
	case WaveformCompression::raw_chunks: return "raw_chunks";
	}
	return "unknown";
}

std::string origin_filter_name(WaveformOriginFilter origin)
{
	switch (origin) {
	case WaveformOriginFilter::all: return "all";
	case WaveformOriginFilter::manual: return "manual";
	case WaveformOriginFilter::power_quality: return "power_quality";
	}
	return "all";
}

WaveformArchiveDiscoveryDto archive_discovery_status(
	const WaveformArchiveDiscoveryStatus &status)
{
	return {
		archive_discovery_state_name(status.state),
		status.scanned_files,
		status.total_files,
		status.rejected_files,
	};
}

WaveformSessionDto waveform_session(const WaveformSessionIpc &session)
{
	return {
		session.id,
		waveform_state_name(session.state),
		session.trigger_sequence,
		session.first_sequence,
		session.last_sequence,
		session.trigger_tai_nanoseconds,
		session.trigger_realtime_nanoseconds,
		session.sample_rate_hz,
		session.event_count,
		waveform_origin(session.trigger_source_mask),
		session.decimation,
		session.filename,
		session.continuation_of_session_id,
		session.master_session_id,
		session.capture_uuid,
		session.format_version,
		waveform_compression(session.compression),
		session.stored_bytes,
		session.logical_sample_bytes,
	};
}

std::uint64_t parse_positive_u64(std::string_view text,
	std::string_view name)
{
	std::uint64_t value = 0u;
	const auto parsed = std::from_chars(
		text.data(), text.data() + text.size(), value);
	if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
	    value == 0u)
		throw std::invalid_argument(
			std::string(name) + " must be a positive integer");
	return value;
}

WaveformListRequest waveform_list_request(std::string_view target)
{
	const auto parameters = query_parameters(target);
	for (const auto &[name, unused] : parameters) {
		(void)unused;
		if (name != "origin" && name != "before_session_id" &&
		    name != "limit")
			throw std::invalid_argument(
				"unsupported waveform query parameter: " + name);
	}

	WaveformListRequest result{};
	if (const auto found = parameters.find("origin");
	    found != parameters.end()) {
		if (found->second == "all")
			result.origin = WaveformOriginFilter::all;
		else if (found->second == "manual")
			result.origin = WaveformOriginFilter::manual;
		else if (found->second == "power_quality")
			result.origin = WaveformOriginFilter::power_quality;
		else
			throw std::invalid_argument(
				"origin must be all, manual, or power_quality");
	}
	if (const auto found = parameters.find("before_session_id");
	    found != parameters.end())
		result.before_session_id = parse_positive_u64(
			found->second, "before_session_id");
	if (const auto found = parameters.find("limit");
	    found != parameters.end()) {
		const auto limit = parse_positive_u64(found->second, "limit");
		if (limit > waveform_max_page_sessions)
			throw std::invalid_argument("limit must be 1..100");
		result.limit = static_cast<std::uint32_t>(limit);
	}
	return result;
}

MncwfUuid waveform_capture_lookup(std::string_view target)
{
	const auto parameters = query_parameters(target);
	if (parameters.size() != 1u || !parameters.contains("capture_uuid"))
		throw std::invalid_argument(
			"required query: capture_uuid=<canonical UUID>");
	const auto uuid = mncwf_uuid_from_string(parameters.at("capture_uuid"));
	if (!uuid || mncwf_uuid_is_zero(*uuid))
		throw std::invalid_argument(
			"capture_uuid must be a nonzero canonical UUID");
	return *uuid;
}

/** Project a daemon waveform response onto the JSON status document. */
WaveformDto waveform_status(const msap1::WaveformResponse &response)
{
	const auto &status = response.waveform;
	WaveformDto result{
		status.running != 0u,
		status.active_session != 0u,
		status.sample_rate_hz,
		status.transport_ring_blocks,
		status.blocks,
		status.frames,
		status.bytes,
		status.invalid_blocks,
		status.sequence_gaps,
		status.transport_overrun_blocks,
		status.materialization_failures,
		status.pl_dropped_frames,
		status.max_capture_frames,
		status.history_oldest_sequence,
		status.history_latest_sequence,
		status.history_capacity_frames,
		status.completed_sessions,
		status.incomplete_sessions,
		status.archive_limit_bytes,
		status.archive_stored_bytes,
		status.expired_sessions,
		status.retention_failures,
		archive_discovery_status(status.archive_discovery),
		{origin_filter_name(response.page.origin),
		 response.page.limit,
		 response.page.total_sessions,
		 response.page.completed_sessions,
		 response.page.incomplete_sessions,
		 response.page.active_sessions,
		 response.page.returned_sessions,
		 response.page.next_before_session_id == 0u
			 ? std::nullopt
			 : std::optional<std::uint64_t>{
				response.page.next_before_session_id}},
		{"mncwf"},
		{},
	};
	result.sessions.reserve(response.sessions.size());
	for (const auto &session : response.sessions)
		result.sessions.push_back(waveform_session(session));
	return result;
}

struct WaveformExportSelection {
	std::uint64_t session_id;
	MncwfUuid event_uuid;
};

WaveformExportSelection export_selection(std::string_view target)
{
	const auto parameters = query_parameters(target);
	if (parameters.size() != 3u || !parameters.contains("session_id") ||
	    !parameters.contains("event_id") || !parameters.contains("format"))
		throw std::invalid_argument(
			"required query: session_id, event_id, format=mncwf");
	if (parameters.at("format") != "mncwf")
		throw std::invalid_argument(
			"unsupported waveform export format; available formats: mncwf");
	std::uint64_t session_id = 0u;
	const auto &session = parameters.at("session_id");
	const auto parsed = std::from_chars(session.data(),
		session.data() + session.size(), session_id);
	if (parsed.ec != std::errc{} ||
	    parsed.ptr != session.data() + session.size() || session_id == 0u)
		throw std::invalid_argument(
			"session_id must be a positive integer");
	const auto event_uuid = mncwf_uuid_from_string(parameters.at("event_id"));
	if (!event_uuid || mncwf_uuid_is_zero(*event_uuid))
		throw std::invalid_argument(
			"event_id must be a nonzero canonical UUID");
	return {session_id, *event_uuid};
}

} // namespace

/**
 * @brief GET /api/v1/waveforms (Viewer)
 *
 * Reports the waveform engine status (transport counters, raw-history
 * bounds) and one bounded page of capture sessions with their state and file
 * name. The exclusive session cursor remains stable while newer captures are
 * added.
 *
 * @return 200 with the status document, or 503 when the acquisition daemon
 *         is unreachable.
 */
webengine::Response get_waveforms(AppContext &app,
				  const webengine::RequestContext &context)
{
	try {
		const auto target = context.request.target();
		const auto request = waveform_list_request(
			std::string_view(target.data(), target.size()));
		const auto response = app.acquisition.waveform_list(request);
		require_acquisition_ok(response.status);
		return json_response(webengine::http::status::ok,
			waveform_status(response));
	} catch (const std::invalid_argument &error) {
		return error_response(webengine::http::status::bad_request,
			error.what());
	} catch (const std::exception &error) {
		log_api_failure("/api/v1/waveforms", error);
		return error_response(
			webengine::http::status::service_unavailable,
			error.what());
	}
}

/**
 * @brief GET /api/v1/waveforms/session?capture_uuid=... (Viewer)
 *
 * Resolves one canonical capture UUID against the full discovered archive,
 * independently of the current waveform-list page.
 */
webengine::Response get_waveform_session(AppContext &app,
					 const webengine::RequestContext &context)
{
	try {
		const auto target = context.request.target();
		const auto capture_uuid = waveform_capture_lookup(
			std::string_view(target.data(), target.size()));
		WaveformLookupRequest request{};
		request.capture_uuid = mncwf_uuid_string(capture_uuid);
		const auto response = app.acquisition.waveform_lookup(request);
		require_acquisition_ok(response.status);

		WaveformSessionLookupDto result{
			request.capture_uuid,
			archive_discovery_status(
				response.waveform.archive_discovery),
			std::nullopt,
		};
		if (response.found != 0u)
			result.session = waveform_session(response.session);
		return json_response(webengine::http::status::ok, result);
	} catch (const std::invalid_argument &error) {
		return error_response(webengine::http::status::bad_request,
			error.what());
	} catch (const std::exception &error) {
		log_api_failure("/api/v1/waveforms/session", error);
		return error_response(
			webengine::http::status::service_unavailable,
			error.what());
	}
}

webengine::Response post_waveform_session_lookup(AppContext &app,
	const webengine::RequestContext &context)
{
	try {
		WaveformBatchLookupRequestDto input{};
		if (const auto error = glz::read_json(input, context.request.body()))
			return error_response(webengine::http::status::bad_request,
				"invalid waveform batch lookup JSON");
		if (input.capture_uuids.empty() || input.capture_uuids.size() > 32u)
			throw std::invalid_argument(
				"waveform batch lookup requires 1..32 capture UUIDs");
		std::unordered_set<std::string> distinct;
		for (const auto &text : input.capture_uuids) {
			const auto uuid = mncwf_uuid_from_string(text);
			if (!uuid || mncwf_uuid_is_zero(*uuid))
				throw std::invalid_argument(
					"capture UUID must be a nonzero canonical UUID");
			if (!distinct.insert(text).second)
				throw std::invalid_argument(
					"waveform batch lookup UUIDs must be distinct");
		}
		WaveformBatchLookupRequest request{};
		request.capture_uuids = std::move(input.capture_uuids);
		const auto response = app.acquisition.waveform_batch_lookup(request);
		require_acquisition_ok(response.status);
		WaveformBatchLookupDto result{
			archive_discovery_status(response.waveform.archive_discovery), {}};
		result.sessions.reserve(response.sessions.size());
		for (const auto &session : response.sessions) {
			if (session)
				result.sessions.emplace_back(waveform_session(*session));
			else
				result.sessions.emplace_back(std::nullopt);
		}
		return json_response(webengine::http::status::ok, result);
	} catch (const std::invalid_argument &error) {
		return error_response(webengine::http::status::bad_request,
			error.what());
	} catch (const std::exception &error) {
		log_api_failure("/api/v1/waveforms/sessions/lookup", error);
		return error_response(webengine::http::status::service_unavailable,
			error.what());
	}
}

/**
 * @brief POST /api/v1/waveforms/trigger (Admin)
 *
 * Starts a manual waveform capture.  Omitted durations use the persistent
 * waveform defaults; the acquisition daemon bounds explicit values against
 * the rate-derived capture budget (and a 120 s per-field sanity cap).
 *
 * @return 200 with the updated waveform status, 400 for invalid JSON or
 *         durations the budget rejects, or 503 when the trigger fails.
 */
webengine::Response post_waveform_trigger(AppContext &app,
					  const webengine::RequestContext &context)
{
	const auto correlation = request_id();
	try {
		WaveformTriggerDto trigger;
		if (const auto error = glz::read_json(
			    trigger, context.request.body()))
			return error_response(
				webengine::http::status::bad_request,
				"invalid waveform trigger JSON");
		const auto response = app.acquisition.trigger_waveform(
			trigger.pretrigger_ms, trigger.posttrigger_ms,
			trigger.decimation,
			msap1::WaveformTriggerSource::manual_web);
		/*
		 * The daemon owns the duration limit because it is
		 * rate-derived (the frame-sized history buffer spans fewer
		 * seconds at higher sample rates). A bad_request reply still
		 * carries the waveform status, so the budget it enforced can
		 * be named here even though IPC rejections are status-only.
		 */
		if (response.status == msap1::AcquisitionStatus::bad_request) {
			const auto &status = response.waveform;
			std::string reason =
				"waveform durations exceed the capture budget";
			if (status.sample_rate_hz > 0u)
				reason += ": pre+post may total at most " +
					std::to_string(
						status.max_capture_frames *
						1000u /
						status.sample_rate_hz) +
					" ms at " +
					std::to_string(status.sample_rate_hz) +
					" frame/s";
			return error_response(
				webengine::http::status::bad_request, reason);
		}
		require_acquisition_ok(response.status);
		log_api_event(mnc::logging::Priority::notice,
			"manual waveform capture triggered",
			"waveform_triggered",
			{{"MNC_REQUEST_ID", correlation},
			 {"MNC_PRETRIGGER_MS",
			  std::to_string(trigger.pretrigger_ms)},
			 {"MNC_POSTTRIGGER_MS",
			  std::to_string(trigger.posttrigger_ms)}});
		return json_response(webengine::http::status::ok,
			waveform_status(response));
	} catch (const std::exception &error) {
		log_api_failure("/api/v1/waveforms/trigger", error);
		return error_response(
			webengine::http::status::service_unavailable,
			error.what());
	}
}

/**
 * @brief DELETE /api/v1/waveforms (Admin)
 *
 * Deletes one completed waveform capture session and its persisted file, or
 * every inactive session when the explicitly confirmed "all" selector is set.
 *
 * @return 200 with the updated waveform status, 400 for invalid JSON or a
 *         missing session ID, or 409 when the daemon rejects the deletion.
 */
webengine::Response
delete_waveform_session(AppContext &app,
			const webengine::RequestContext &context)
{
	const auto correlation = request_id();
	try {
		WaveformDeleteDto deletion;
		if (const auto error = glz::read_json(
			    deletion, context.request.body()))
			return error_response(
				webengine::http::status::bad_request,
				"invalid waveform deletion JSON");
		if (deletion.all && deletion.session_id != 0u)
			return error_response(
				webengine::http::status::bad_request,
				"select waveform session_id or all, but not both");
		if (deletion.all && !deletion.confirmed)
			return error_response(
				webengine::http::status::bad_request,
				"bulk waveform deletion requires explicit confirmation");
		if (!deletion.all && deletion.session_id == 0u)
			return error_response(
				webengine::http::status::bad_request,
				"waveform session ID is required");

		WaveformListRequest list_request{};
		list_request.origin = WaveformOriginFilter::all;
		list_request.limit = waveform_max_page_sessions;
		auto response = app.acquisition.waveform_list(list_request);
		require_acquisition_ok(response.status);
		std::vector<std::uint64_t> targets;
		std::uint64_t deleted = 0u;
		if (deletion.all) {
			if (response.waveform.archive_discovery.state !=
			    WaveformArchiveDiscoveryState::complete)
				return error_response(webengine::http::status::conflict,
					"waveform archive indexing must complete before all data can be cleared");
			if (response.waveform.active_session != 0u ||
			    std::ranges::any_of(response.sessions,
				    [](const auto &session) {
					    return session.state ==
						    WaveformSessionState::capturing;
				    }))
				return error_response(webengine::http::status::conflict,
					"all waveform data cannot be cleared while a capture is active");
		} else {
			targets.push_back(deletion.session_id);
		}
		do {
			if (deletion.all) {
				targets.clear();
				for (const auto &session : response.sessions)
					targets.push_back(session.id);
			}
			for (const auto session_id : targets) {
				response = app.acquisition.delete_waveform(session_id);
				require_acquisition_ok(response.status);
				++deleted;
			}
		} while (deletion.all && !targets.empty() &&
			 !response.sessions.empty());
		log_api_event(mnc::logging::Priority::notice,
			deletion.all ? "all waveform captures deleted"
				: "waveform capture deleted",
			deletion.all ? "waveforms_cleared" : "waveform_deleted",
			{{"MNC_REQUEST_ID", correlation},
			 {deletion.all ? "MNC_WAVEFORMS_DELETED"
				: "MNC_WAVEFORM_SESSION",
			  std::to_string(deletion.all ? deleted
				: deletion.session_id)}});
		return json_response(webengine::http::status::ok,
			waveform_status(response));
	} catch (const std::exception &error) {
		log_api_failure("/api/v1/waveforms", error);
		return error_response(webengine::http::status::conflict,
			error.what());
	}
}

webengine::HandlerResult export_waveform_event(
	AppContext &app, const webengine::RequestContext &context)
{
	WaveformExportSelection selection{};
	try {
		const auto target = context.request.target();
		selection = export_selection(
			std::string_view(target.data(), target.size()));
	} catch (const std::invalid_argument &error) {
		return error_response(webengine::http::status::bad_request,
			error.what());
	}

	try {
		WaveformLookupRequest request{};
		request.session_id = selection.session_id;
		const auto response = app.acquisition.waveform_lookup(request);
		require_acquisition_ok(response.status);
		if (response.found == 0u)
			return error_response(webengine::http::status::not_found,
				"waveform session was not found");
		if (response.session.state != WaveformSessionState::complete ||
		    response.session.filename.empty())
			return error_response(webengine::http::status::conflict,
				"waveform session is not a completed capture");
		if (response.waveform_directory.empty())
			throw std::runtime_error(
				"acquisition daemon returned no waveform directory");

		auto file = MncwfV4ExportFile::open(response.waveform_directory,
			response.session.filename, selection.event_uuid);
		const auto event_text = mncwf_uuid_string(selection.event_uuid);
		const auto download_name = "waveform-" +
			std::to_string(selection.session_id) + "-event-" +
			event_text + ".mncwf";
		log_api_event(mnc::logging::Priority::info,
			"MNCWF event export opened", "waveform_export_opened",
			{{"MNC_WAVEFORM_SESSION",
			  std::to_string(selection.session_id)},
			 {"MNC_EVENT_ID", event_text},
			 {"MNC_EXPORT_FORMAT", "mncwf"}});
		return webengine::StreamingDownload{
			download_name,
			"application/x-mncwf",
			file->size(),
			[file = std::move(file)](std::uint64_t offset,
				std::span<std::byte> destination) {
				return file->read(offset, destination);
			}};
	} catch (const AcquisitionUnavailable &error) {
		log_api_failure("/api/v1/waveforms/export", error);
		return error_response(webengine::http::status::service_unavailable,
			error.what());
	} catch (const std::system_error &error) {
		log_api_failure("/api/v1/waveforms/export", error);
		return error_response(webengine::http::status::conflict,
			"waveform master is unavailable: " +
				std::string(error.what()));
	} catch (const std::exception &error) {
		log_api_failure("/api/v1/waveforms/export", error);
		return error_response(webengine::http::status::unprocessable_entity,
			"waveform master cannot produce this event export: " +
				std::string(error.what()));
	}
}

void document_waveform_routes(DocumentedApiRegistry &registry)
{
	using Verb = webengine::http::verb;
	constexpr std::string_view waveforms = "/api/v1/waveforms";
	registry.add_query_parameter(Verb::get, waveforms, "origin", "string",
		false, "Capture origin filter", {"all", "manual", "power_quality"},
		"all");
	registry.add_query_parameter(Verb::get, waveforms, "before_session_id",
		"integer", false, "Exclusive session pagination cursor");
	registry.add_query_parameter(Verb::get, waveforms, "limit", "integer",
		false, "Maximum sessions to return, from 1 through 100", {}, "100");
	registry.add_json_response<WaveformDto>(Verb::get, waveforms, 200,
		"WaveformStatus", "Waveform engine status and capture page");
	registry.add_error_response(Verb::get, waveforms, 400,
		"The pagination or origin query is invalid");
	registry.add_error_response(Verb::get, waveforms, 503,
		"Waveform acquisition is unavailable");

	constexpr std::string_view lookup = "/api/v1/waveforms/session";
	registry.add_query_parameter(Verb::get, lookup, "capture_uuid", "string",
		true, "Canonical nonzero capture UUID", {},
		"d2f78547-4d73-46c2-bc69-c9cc763cc15a");
	registry.add_json_response<WaveformSessionLookupDto>(Verb::get, lookup, 200,
		"WaveformSessionLookup", "Archive lookup result");
	registry.add_error_response(Verb::get, lookup, 400,
		"The capture UUID is absent or malformed");
	registry.add_error_response(Verb::get, lookup, 503,
		"Waveform acquisition is unavailable");

	constexpr std::string_view batch_lookup =
		"/api/v1/waveforms/sessions/lookup";
	registry.add_json_request<WaveformBatchLookupRequestDto>(Verb::post,
		batch_lookup, "WaveformBatchLookup", "One through 32 distinct capture UUIDs",
		true, R"({"capture_uuids":["d2f78547-4d73-46c2-bc69-c9cc763cc15a"]})");
	registry.add_json_response<WaveformBatchLookupDto>(Verb::post, batch_lookup,
		200, "WaveformBatchLookupResult", "Ordered retained session or null results");
	registry.add_error_response(Verb::post, batch_lookup, 400,
		"The UUID list is empty, oversized, duplicated, or malformed");
	registry.add_error_response(Verb::post, batch_lookup, 503,
		"Waveform acquisition is unavailable");

	constexpr std::string_view trigger = "/api/v1/waveforms/trigger";
	registry.add_json_request<WaveformTriggerDto>(Verb::post, trigger,
		"WaveformTrigger", "Manual capture durations and decimation", true,
		R"({"pretrigger_ms":500,"posttrigger_ms":1000,"decimation":1})");
	registry.add_json_response<WaveformDto>(Verb::post, trigger, 200,
		"WaveformStatus", "Updated waveform status");
	registry.add_error_response(Verb::post, trigger, 400,
		"The request or capture duration is invalid");
	registry.add_error_response(Verb::post, trigger, 503,
		"The capture could not be started");

	registry.add_json_request<WaveformDeleteDto>(Verb::delete_, waveforms,
		"WaveformDelete", "Session selection and bulk-delete confirmation",
		true, R"({"session_id":42,"all":false,"confirmed":false})");
	registry.add_json_response<WaveformDto>(Verb::delete_, waveforms, 200,
		"WaveformStatus", "Updated waveform status");
	registry.add_error_response(Verb::delete_, waveforms, 400,
		"The deletion selection is invalid");
	registry.add_error_response(Verb::delete_, waveforms, 409,
		"The selected waveform cannot be deleted");

	constexpr std::string_view export_path = "/api/v1/waveforms/export";
	registry.add_query_parameter(Verb::get, export_path, "session_id",
		"integer", true, "Completed waveform session ID", {}, "42");
	registry.add_query_parameter(Verb::get, export_path, "event_id", "string",
		true, "Canonical event UUID within the waveform", {},
		"d2f78547-4d73-46c2-bc69-c9cc763cc15a");
	registry.add_query_parameter(Verb::get, export_path, "format", "string",
		true, "Requested virtual export format", {"mncwf"}, "mncwf");
	registry.add_binary_response(Verb::get, export_path, 200,
		"application/x-mncwf", "Virtual event-specific waveform capture");
	registry.add_error_response(Verb::get, export_path, 400,
		"The export query is invalid");
	registry.add_error_response(Verb::get, export_path, 404,
		"The waveform session does not exist");
	registry.add_error_response(Verb::get, export_path, 409,
		"The waveform is incomplete or unavailable");
	registry.add_error_response(Verb::get, export_path, 422,
		"The event cannot be projected from the waveform");
	registry.add_error_response(Verb::get, export_path, 503,
		"Waveform acquisition is unavailable");

	for (const auto &[path, attachment] : {
		std::pair<std::string_view, bool>{
			"/protected/waveforms/view/{filename}", false},
		std::pair<std::string_view, bool>{
			"/protected/waveforms/download/{filename}", true}}) {
		registry.add_binary_response(Verb::get, path, 200,
			"application/x-mncwf", "Retained waveform capture", {},
			attachment);
		registry.add_error_response(Verb::get, path, 404,
			"The retained capture does not exist");
	}
}

} // namespace msap1::web::api
