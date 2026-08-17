/**
 * @file waveform_routes.cpp
 * @brief Waveform endpoints: engine status, manual capture triggers, and
 *        session deletion.
 */

#include "health_dto.hpp"
#include "response.hpp"
#include "routes.hpp"

#include <cstdint>
#include <exception>
#include <string>
#include <vector>

#include <glaze/glaze.hpp>

namespace msap1::web::api {

namespace {

/** Body of POST /api/v1/waveforms/trigger. */
struct WaveformTriggerDto {
	std::uint32_t pretrigger_ms = msap1::waveform_duration_unspecified;
	std::uint32_t posttrigger_ms = msap1::waveform_duration_unspecified;
};

/** Body of DELETE /api/v1/waveforms. */
struct WaveformDeleteDto {
	std::uint64_t session_id = 0;
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
	std::string filename;
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
	std::vector<WaveformSessionDto> sessions;
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
		{},
	};
	result.sessions.reserve(response.sessions.size());
	for (const auto &session : response.sessions) {
		result.sessions.push_back({
			session.id,
			waveform_state_name(session.state),
			session.trigger_sequence,
			session.first_sequence,
			session.last_sequence,
			session.trigger_tai_nanoseconds,
			session.trigger_realtime_nanoseconds,
			session.sample_rate_hz,
			session.event_count,
			session.filename,
		});
	}
	return result;
}

} // namespace

/**
 * @brief GET /api/v1/waveforms (Viewer)
 *
 * Reports the waveform engine status (transport counters, raw-history
 * bounds) and every known capture session with its state and file name.
 *
 * @return 200 with the status document, or 503 when the acquisition daemon
 *         is unreachable.
 */
webengine::Response get_waveforms(AppContext &app,
				  const webengine::RequestContext &)
{
	try {
		const auto response = app.acquisition.waveform_status();
		require_acquisition_ok(response.status);
		return json_response(webengine::http::status::ok,
			waveform_status(response));
	} catch (const std::exception &error) {
		log_api_failure("/api/v1/waveforms", error);
		return error_response(
			webengine::http::status::service_unavailable,
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
 * Deletes one completed waveform capture session and its persisted file.
 * The session is identified by the body's non-zero "session_id".
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
		if (deletion.session_id == 0u)
			return error_response(
				webengine::http::status::bad_request,
				"waveform session ID is required");
		const auto response =
			app.acquisition.delete_waveform(deletion.session_id);
		require_acquisition_ok(response.status);
		log_api_event(mnc::logging::Priority::notice,
			"waveform capture deleted",
			"waveform_deleted",
			{{"MNC_REQUEST_ID", correlation},
			 {"MNC_WAVEFORM_SESSION",
			  std::to_string(deletion.session_id)}});
		return json_response(webengine::http::status::ok,
			waveform_status(response));
	} catch (const std::exception &error) {
		log_api_failure("/api/v1/waveforms", error);
		return error_response(webengine::http::status::conflict,
			error.what());
	}
}

} // namespace msap1::web::api
