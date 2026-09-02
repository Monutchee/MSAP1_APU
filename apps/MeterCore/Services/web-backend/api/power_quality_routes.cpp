/** M18 durable event, flicker, and mains-signalling product endpoints. */

#include "health_dto.hpp"
#include "meter_dto.hpp"
#include "openapi.hpp"
#include "query.hpp"
#include "response.hpp"
#include "routes.hpp"

#include "msap1/waveform/mncwf_v4.hpp"

#include <array>
#include <charconv>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace msap1::web::api {
namespace {

constexpr std::array<const char *, 3> phase_names{"A", "B", "C"};

std::uint32_t parse_limit(std::string_view text)
{
	std::uint32_t value = 0u;
	const auto parsed = std::from_chars(text.data(), text.data() + text.size(),
		value);
	if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
	    value == 0u || value > 1000u)
		throw std::invalid_argument("limit must be 1..1000");
	return value;
}

std::int64_t parse_nanoseconds(std::string_view text,
	std::string_view name)
{
	std::int64_t value = 0;
	const auto parsed = std::from_chars(text.data(), text.data() + text.size(),
		value);
	if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
		throw std::invalid_argument(std::string(name) +
			" must be signed decimal nanoseconds");
	return value;
}

history::PowerQualityEventQuery event_query(std::string_view target)
{
	const auto parameters = query_parameters(target);
	for (const auto &[name, unused] : parameters) {
		(void)unused;
		if (name != "event_id" && name != "start_utc_ns" &&
		    name != "end_utc_ns" && name != "limit")
			throw std::invalid_argument(
				"unsupported event query parameter: " + name);
	}
	history::PowerQualityEventQuery result{};
	result.limit = 100u;
	if (const auto found = parameters.find("event_id");
	    found != parameters.end()) {
		const auto uuid = mncwf_uuid_from_string(found->second);
		if (!uuid || mncwf_uuid_is_zero(*uuid))
			throw std::invalid_argument(
				"event_id must be a nonzero canonical UUID");
		result.event_uuid = *uuid;
		result.limit = 1u;
	}
	if (const auto found = parameters.find("start_utc_ns");
	    found != parameters.end())
		result.start_utc_nanoseconds = parse_nanoseconds(
			found->second, "start_utc_ns");
	if (const auto found = parameters.find("end_utc_ns");
	    found != parameters.end())
		result.end_utc_nanoseconds = parse_nanoseconds(
			found->second, "end_utc_ns");
	if (const auto found = parameters.find("limit");
	    found != parameters.end()) {
		if (result.event_uuid)
			throw std::invalid_argument(
				"limit is not valid with an event_id detail query");
		result.limit = parse_limit(found->second);
	}
	if (result.start_utc_nanoseconds && result.end_utc_nanoseconds &&
	    *result.start_utc_nanoseconds > *result.end_utc_nanoseconds)
		throw std::invalid_argument("event UTC range is reversed");
	return result;
}

std::string event_lifecycle_name(PowerQualityEventLifecycle lifecycle)
{
	switch (lifecycle) {
	case PowerQualityEventLifecycle::start: return "start";
	case PowerQualityEventLifecycle::update: return "update";
	case PowerQualityEventLifecycle::end: return "end";
	case PowerQualityEventLifecycle::abort: return "abort";
	}
	return "unknown";
}

std::string event_type_name(PowerQualityLifecycleType type)
{
	switch (type) {
	case PowerQualityLifecycleType::voltage_sag: return "voltage_sag";
	case PowerQualityLifecycleType::voltage_swell: return "voltage_swell";
	case PowerQualityLifecycleType::voltage_interruption:
		return "voltage_interruption";
	case PowerQualityLifecycleType::rapid_voltage_change:
		return "rapid_voltage_change";
	case PowerQualityLifecycleType::voltage_unbalance:
		return "voltage_unbalance";
	case PowerQualityLifecycleType::current_sag: return "current_sag";
	case PowerQualityLifecycleType::current_swell: return "current_swell";
	case PowerQualityLifecycleType::current_unbalance:
		return "current_unbalance";
	case PowerQualityLifecycleType::transient_voltage:
		return "transient_voltage";
	}
	return "unknown";
}

std::vector<std::string> phases(std::uint8_t mask)
{
	std::vector<std::string> result;
	for (std::size_t phase = 0; phase < phase_names.size(); ++phase)
		if ((mask & (1u << phase)) != 0u)
			result.emplace_back(phase_names[phase]);
	return result;
}

std::string settings_digest(const std::array<std::uint32_t, 4> &words)
{
	std::ostringstream output;
	output << std::hex << std::setfill('0');
	for (const auto word : words)
		output << std::setw(8) << word;
	return output.str();
}

struct EventWaveformPolicyDto {
	bool enabled = false;
	std::uint32_t pretrigger_ms = 0;
	std::uint32_t posttrigger_ms = 0;
	std::uint32_t decimation = 1;
};

struct PowerQualityEventDto {
	std::string event_id;
	std::uint64_t source_session = 0;
	std::uint64_t source_counter = 0;
	std::string lifecycle;
	std::string type;
	std::string taxonomy;
	std::vector<std::string> affected_phases;
	std::uint8_t trigger_source = 0;
	std::uint32_t sequence = 0;
	std::uint32_t configuration_generation = 0;
	std::uint32_t profile_generation = 0;
	std::uint32_t sample_rate_hz = 0;
	std::uint64_t first_sample = 0;
	std::uint64_t last_sample = 0;
	std::uint64_t trigger_sample = 0;
	std::uint64_t duration_samples = 0;
	double duration_ms = 0.0;
	std::uint32_t threshold_e4 = 0;
	std::uint32_t hysteresis_e4 = 0;
	std::uint32_t reference_micro_units = 0;
	std::array<std::uint32_t, 3> minimum_micro_units{};
	std::array<std::uint32_t, 3> maximum_micro_units{};
	std::array<std::uint32_t, 3> current_micro_units{};
	bool per_phase = false;
	std::uint32_t status = 0;
	std::uint32_t valid_mask = 0;
	std::uint32_t discontinuities = 0;
	std::uint32_t update_count = 0;
	std::string time_quality;
	std::optional<std::int64_t> start_utc_nanoseconds;
	std::optional<std::int64_t> last_utc_nanoseconds;
	std::optional<std::uint64_t> utc_uncertainty_nanoseconds;
	std::string settings_digest;
	EventWaveformPolicyDto waveform;
	std::vector<std::string> waveform_capture_uuids;
};

PowerQualityEventDto event_dto(
	const history::PowerQualityEventCatalogEntry &entry)
{
	const auto &event = entry.event;
	PowerQualityEventDto result{};
	result.event_id = mncwf_uuid_string(entry.event_uuid);
	result.source_session = event.id.session;
	result.source_counter = event.id.counter;
	result.lifecycle = event_lifecycle_name(event.lifecycle);
	result.type = event_type_name(event.type);
	result.taxonomy = event.iec_classification
		? "iec_61000_4_30" : "msap1_product_alarm";
	result.affected_phases = phases(event.phase_mask);
	result.trigger_source = event.trigger_source;
	result.sequence = event.sequence;
	result.configuration_generation = event.configuration_generation;
	result.profile_generation = event.profile_generation;
	result.sample_rate_hz = event.sample_rate_hz;
	result.first_sample = event.first_sample;
	result.last_sample = event.last_sample;
	result.trigger_sample = event.trigger_sample;
	result.duration_samples = event.duration_samples;
	result.duration_ms = event.sample_rate_hz == 0u ? 0.0
		: static_cast<double>(event.duration_samples) * 1000.0 /
			static_cast<double>(event.sample_rate_hz);
	result.threshold_e4 = event.threshold_e4;
	result.hysteresis_e4 = event.hysteresis_e4;
	result.reference_micro_units = event.reference_micro_units;
	result.minimum_micro_units = event.minimum_micro_units;
	result.maximum_micro_units = event.maximum_micro_units;
	result.current_micro_units = event.current_micro_units;
	result.per_phase = event.per_phase;
	result.status = event.status;
	result.valid_mask = event.valid_mask;
	result.discontinuities = event.discontinuities;
	result.update_count = event.update_count;
	result.time_quality = time_quality_name(event.time_quality);
	result.start_utc_nanoseconds = entry.start_utc_nanoseconds;
	result.last_utc_nanoseconds = entry.last_utc_nanoseconds;
	result.utc_uncertainty_nanoseconds = entry.utc_uncertainty_nanoseconds;
	result.settings_digest = settings_digest(event.settings_digest);
	result.waveform = {event.waveform_enabled,
		event.waveform_pretrigger_ms, event.waveform_posttrigger_ms,
		event.waveform_decimation};
	result.waveform_capture_uuids.reserve(
		entry.waveform_capture_uuids.size());
	for (const auto &uuid : entry.waveform_capture_uuids)
		result.waveform_capture_uuids.push_back(mncwf_uuid_string(uuid));
	return result;
}

struct PowerQualityEventsDto {
	std::uint32_t limit = 0;
	std::uint32_t count = 0;
	std::vector<std::string> export_formats{"mncwf"};
	std::vector<PowerQualityEventDto> events;
};

struct PowerQualityEventDeleteDto {
	std::vector<std::string> event_ids;
	bool all = false;
	bool confirmed = false;
};

struct PowerQualityEventDeleteResultDto {
	std::uint64_t deleted = 0;
};

struct FlickerPhaseDto {
	std::string phase;
	bool valid = false;
	double pinst = 0.0;
	double pst = 0.0;
	double plt = 0.0;
	std::uint32_t valid_internal_samples = 0;
};

struct FlickerRecordDto {
	std::string kind;
	std::uint32_t sequence = 0;
	std::uint32_t configuration_generation = 0;
	std::uint32_t profile_generation = 0;
	std::uint32_t sample_rate_hz = 0;
	std::uint64_t first_sample = 0;
	std::uint64_t last_sample = 0;
	std::uint32_t sample_count = 0;
	std::uint32_t interval_seconds = 0;
	std::uint16_t lamp_voltage = 0;
	std::uint8_t nominal_frequency_hz = 0;
	std::uint32_t status = 0;
	std::uint32_t source_status = 0;
	std::vector<FlickerPhaseDto> phases;
};

FlickerRecordDto flicker_record(const FlickerSnapshot &snapshot)
{
	const auto kind = snapshot.kind == FlickerRecordKind::live ? "live"
		: snapshot.kind == FlickerRecordKind::pst ? "pst" : "plt";
	FlickerRecordDto result{kind, snapshot.sequence,
		snapshot.configuration_generation, snapshot.profile_generation,
		snapshot.sample_rate_hz, snapshot.first_sample, snapshot.last_sample,
		snapshot.sample_count, snapshot.interval_seconds,
		snapshot.lamp_voltage, snapshot.nominal_frequency_hz,
		snapshot.status, snapshot.source_status, {}};
	result.phases.reserve(3u);
	for (std::size_t phase = 0; phase < 3u; ++phase)
		result.phases.push_back({phase_names[phase],
			(snapshot.phase_valid_mask & (1u << phase)) != 0u,
			static_cast<double>(snapshot.pinst_q16[phase]) / 65536.0,
			static_cast<double>(snapshot.pst_q16[phase]) / 65536.0,
			static_cast<double>(snapshot.plt_q16[phase]) / 65536.0,
			snapshot.valid_internal_samples[phase]});
	return result;
}

struct FlickerDto {
	bool running = false;
	std::uint64_t records = 0;
	std::uint64_t sequence_gaps = 0;
	std::optional<FlickerRecordDto> live;
	std::optional<FlickerRecordDto> pst;
	std::optional<FlickerRecordDto> plt;
};

struct MainsSignalPhaseDto {
	std::string phase;
	bool valid = false;
	bool detected = false;
	double magnitude_volts = 0.0;
	double background_volts = 0.0;
};

struct MainsSignalDto {
	bool running = false;
	std::uint64_t records = 0;
	std::uint64_t sequence_gaps = 0;
	bool available = false;
	std::uint32_t sequence = 0;
	std::uint32_t configuration_generation = 0;
	std::uint32_t profile_generation = 0;
	std::uint32_t sample_rate_hz = 0;
	std::uint64_t first_sample = 0;
	std::uint64_t last_sample = 0;
	std::uint32_t sample_count = 0;
	double configured_hz = 0.0;
	double measured_hz = 0.0;
	double bandwidth_hz = 0.0;
	std::uint32_t observation_ms = 0;
	double threshold_percent = 0.0;
	double reference_volts = 0.0;
	std::uint32_t status = 0;
	std::uint32_t source_status = 0;
	std::vector<MainsSignalPhaseDto> phases;
};

MainsSignalDto mains_signal_dto(const MainsSignalResponse &response)
{
	MainsSignalDto result{};
	result.running = response.running;
	result.records = response.records;
	result.sequence_gaps = response.sequence_gaps;
	result.available = response.has_snapshot;
	if (!response.has_snapshot)
		return result;
	const auto &snapshot = response.snapshot;
	result.sequence = snapshot.sequence;
	result.configuration_generation = snapshot.configuration_generation;
	result.profile_generation = snapshot.profile_generation;
	result.sample_rate_hz = snapshot.sample_rate_hz;
	result.first_sample = snapshot.first_sample;
	result.last_sample = snapshot.last_sample;
	result.sample_count = snapshot.sample_count;
	result.configured_hz = static_cast<double>(snapshot.configured_millihz) /
		1000.0;
	result.measured_hz = static_cast<double>(snapshot.measured_millihz) /
		1000.0;
	result.bandwidth_hz = static_cast<double>(snapshot.bandwidth_millihz) /
		1000.0;
	result.observation_ms = snapshot.observation_ms;
	result.threshold_percent = static_cast<double>(snapshot.threshold_e4) /
		100.0;
	result.reference_volts =
		static_cast<double>(snapshot.reference_microvolts) / 1e6;
	result.status = snapshot.status;
	result.source_status = snapshot.source_status;
	result.phases.reserve(3u);
	for (std::size_t phase = 0; phase < 3u; ++phase)
		result.phases.push_back({phase_names[phase],
			(snapshot.phase_valid_mask & (1u << phase)) != 0u,
			(snapshot.detected_phase_mask & (1u << phase)) != 0u,
			static_cast<double>(snapshot.magnitude_microvolts[phase]) / 1e6,
			static_cast<double>(snapshot.background_microvolts[phase]) / 1e6});
	return result;
}

} // namespace

webengine::Response get_power_quality_events(AppContext &app,
	const webengine::RequestContext &request)
{
	try {
		const auto target = request.request.target();
		const auto query = event_query(
			std::string_view(target.data(), target.size()));
		const auto entries = app.database.query_power_quality_events(query);
		if (query.event_uuid && entries.empty())
			return error_response(webengine::http::status::not_found,
				"power-quality event does not exist");
		PowerQualityEventsDto result{};
		result.limit = query.limit;
		result.count = static_cast<std::uint32_t>(entries.size());
		result.events.reserve(entries.size());
		for (const auto &entry : entries)
			result.events.push_back(event_dto(entry));
		return json_response(webengine::http::status::ok, result);
	} catch (const std::invalid_argument &error) {
		return error_response(webengine::http::status::bad_request,
			error.what());
	} catch (const std::exception &error) {
		log_api_failure("/api/v1/meter/power-quality/events", error);
		return error_response(webengine::http::status::service_unavailable,
			error.what());
	}
}

webengine::Response delete_power_quality_events(AppContext &app,
	const webengine::RequestContext &request)
{
	const auto correlation = request_id();
	try {
		PowerQualityEventDeleteDto selection;
		if (glz::read_json(selection, request.request.body()))
			return error_response(webengine::http::status::bad_request,
				"invalid power-quality event deletion JSON");
		if (!selection.confirmed)
			return error_response(webengine::http::status::bad_request,
				"power-quality event deletion requires explicit confirmation");
		if (selection.all == !selection.event_ids.empty())
			return error_response(webengine::http::status::bad_request,
				"select event_ids or all, but not both");
		if (selection.event_ids.size() > 1000u)
			return error_response(webengine::http::status::bad_request,
				"at most 1000 power-quality events may be deleted at once");

		std::vector<PowerQualityEventUuid> event_uuids;
		event_uuids.reserve(selection.event_ids.size());
		for (const auto &text : selection.event_ids) {
			const auto uuid = mncwf_uuid_from_string(text);
			if (!uuid || mncwf_uuid_is_zero(*uuid))
				return error_response(
					webengine::http::status::bad_request,
					"event_ids must contain nonzero canonical UUIDs");
			event_uuids.push_back(*uuid);
		}
		const auto deleted = selection.all
			? app.database.clear_power_quality_events()
			: app.database.delete_power_quality_events(event_uuids);
		log_api_event(mnc::logging::Priority::notice,
			"power-quality catalogue events deleted",
			"power_quality_events_deleted",
			{{"MNC_REQUEST_ID", correlation},
			 {"MNC_PQ_EVENTS_DELETED", std::to_string(deleted)}});
		return json_response(webengine::http::status::ok,
			PowerQualityEventDeleteResultDto{deleted});
	} catch (const std::invalid_argument &error) {
		return error_response(webengine::http::status::bad_request,
			error.what());
	} catch (const std::exception &error) {
		log_api_failure("/api/v1/meter/power-quality/events", error);
		return error_response(webengine::http::status::conflict,
			error.what());
	}
}

webengine::Response get_meter_flicker(AppContext &app,
	const webengine::RequestContext &)
{
	try {
		const auto response = app.acquisition.flicker();
		require_acquisition_ok(response.status);
		FlickerDto result{response.running, response.records,
			response.sequence_gaps, {}, {}, {}};
		if (response.has_live) result.live = flicker_record(response.live);
		if (response.has_pst) result.pst = flicker_record(response.pst);
		if (response.has_plt) result.plt = flicker_record(response.plt);
		return json_response(webengine::http::status::ok, result);
	} catch (const std::exception &error) {
		log_api_failure("/api/v1/meter/flicker", error);
		return error_response(webengine::http::status::service_unavailable,
			error.what());
	}
}

webengine::Response get_meter_mains_signalling(AppContext &app,
	const webengine::RequestContext &)
{
	try {
		const auto response = app.acquisition.mains_signalling();
		require_acquisition_ok(response.status);
		return json_response(webengine::http::status::ok,
			mains_signal_dto(response));
	} catch (const std::exception &error) {
		log_api_failure("/api/v1/meter/mains-signalling", error);
		return error_response(webengine::http::status::service_unavailable,
			error.what());
	}
}

void document_power_quality_routes(DocumentedApiRegistry &registry)
{
	using Verb = webengine::http::verb;
	constexpr std::string_view events =
		"/api/v1/meter/power-quality/events";
	registry.add_query_parameter(Verb::get, events, "event_id", "string",
		false, "Return one event with this canonical UUID", {},
		"d2f78547-4d73-46c2-bc69-c9cc763cc15a");
	registry.add_query_parameter(Verb::get, events, "start_utc_ns", "integer",
		false, "Inclusive UTC range start in nanoseconds");
	registry.add_query_parameter(Verb::get, events, "end_utc_ns", "integer",
		false, "Inclusive UTC range end in nanoseconds");
	registry.add_query_parameter(Verb::get, events, "limit", "integer", false,
		"Maximum number of events, from 1 through 1000", {}, "100");
	registry.add_json_response<PowerQualityEventsDto>(Verb::get, events, 200,
		"PowerQualityEvents", "Power-quality event catalogue page");
	registry.add_error_response(Verb::get, events, 400,
		"The event query is invalid");
	registry.add_error_response(Verb::get, events, 404,
		"The requested event does not exist");
	registry.add_error_response(Verb::get, events, 503,
		"The event catalogue is unavailable");

	registry.add_json_request<PowerQualityEventDeleteDto>(Verb::delete_, events,
		"PowerQualityEventDelete", "Selection and explicit confirmation");
	registry.add_json_response<PowerQualityEventDeleteResultDto>(Verb::delete_,
		events, 200, "PowerQualityEventDeleteResult",
		"Number of catalogue events deleted");
	registry.add_error_response(Verb::delete_, events, 400,
		"The deletion request is invalid or unconfirmed");
	registry.add_error_response(Verb::delete_, events, 409,
		"The catalogue could not complete the deletion");

	registry.add_json_response<FlickerDto>(Verb::get,
		"/api/v1/meter/flicker", 200, "Flicker",
		"Latest live, Pst, and Plt flicker records");
	registry.add_error_response(Verb::get, "/api/v1/meter/flicker", 503,
		"Flicker acquisition is unavailable");
	registry.add_json_response<MainsSignalDto>(Verb::get,
		"/api/v1/meter/mains-signalling", 200, "MainsSignalling",
		"Latest mains-signalling carrier observation");
	registry.add_error_response(Verb::get,
		"/api/v1/meter/mains-signalling", 503,
		"Mains-signalling acquisition is unavailable");
}

} // namespace msap1::web::api
