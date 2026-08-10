/** @file history_routes.cpp Bounded typed historian query endpoints. */

#include "response.hpp"
#include "routes.hpp"

#include "mnc/MeterDataProvider/attributes/meter_attribute.hpp"

#include <array>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <glaze/glaze.hpp>

namespace msap1::web::api {
namespace {

using Attribute = mnc::meter::MeterAttributeId;

struct HistoryQueryDto {
	std::string period = "basic";
	std::vector<std::string> attributes;
	std::int64_t start_nanoseconds = 0;
	std::int64_t end_nanoseconds = 0;
	std::uint32_t limit = 10000;
};

struct HistoryPointDto {
	std::int64_t measured_at_nanoseconds = 0;
	std::uint64_t source_sequence = 0;
	std::string attribute;
	std::int64_t value = 0;
	std::string quality;
};

struct HistoryResponseDto {
	std::string period;
	std::vector<HistoryPointDto> points;
	bool truncated = false;
};

struct AttributeCapabilityDto {
	std::string id;
	std::string unit;
};

struct HistoryCapabilitiesDto {
	std::vector<std::string> periods;
	std::vector<AttributeCapabilityDto> attributes;
	std::uint32_t maximum_points = 50000;
};

constexpr std::array<Attribute, 8> historical_attributes = {
	Attribute::Frequency, Attribute::VanRms, Attribute::VbnRms,
	Attribute::VcnRms, Attribute::IaRms, Attribute::IbRms,
	Attribute::IcRms, Attribute::InRms};

std::string unit_name(mnc::meter::MeterUnit unit)
{
	switch (unit) {
	case mnc::meter::MeterUnit::MilliHertz: return "mHz";
	case mnc::meter::MeterUnit::MicroVolts: return "uV";
	case mnc::meter::MeterUnit::MicroAmperes: return "uA";
	}
	return "unknown";
}

std::string quality_name(MeasurementQuality quality)
{
	switch (quality) {
	case MeasurementQuality::unavailable: return "unavailable";
	case MeasurementQuality::valid: return "valid";
	case MeasurementQuality::invalid: return "invalid";
	case MeasurementQuality::out_of_range: return "out_of_range";
	case MeasurementQuality::timed_out: return "timed_out";
	case MeasurementQuality::arithmetic_error: return "arithmetic_error";
	}
	return "unavailable";
}

MeasurementPeriod parse_period(std::string_view value)
{
	if (value == "basic") return MeasurementPeriod::Basic;
	if (value == "cycles_150_180") return MeasurementPeriod::Cycles150_180;
	if (value == "minutes_10") return MeasurementPeriod::Min10;
	if (value == "hours_2") return MeasurementPeriod::Hour2;
	throw std::invalid_argument("unsupported history measurement period");
}

Attribute parse_attribute(std::string_view value)
{
	for (const auto attribute : historical_attributes) {
		if (mnc::meter::describe({attribute, std::nullopt}).key == value)
			return attribute;
	}
	throw std::invalid_argument("unsupported history meter attribute");
}

std::string period_name(MeasurementPeriod period)
{
	switch (period) {
	case MeasurementPeriod::Basic: return "basic";
	case MeasurementPeriod::Cycles150_180: return "cycles_150_180";
	case MeasurementPeriod::Min10: return "minutes_10";
	case MeasurementPeriod::Hour2: return "hours_2";
	}
	throw std::invalid_argument("unsupported historian period capability");
}

HistoryCapabilitiesDto capabilities(
	const history::HistorianCapabilities &source)
{
	HistoryCapabilitiesDto result;
	result.maximum_points = source.maximum_points;
	for (const auto period : source.periods)
		result.periods.push_back(period_name(period));
	for (const auto attribute : source.attributes) {
		const auto descriptor = mnc::meter::describe(
			{attribute, std::nullopt});
		result.attributes.push_back(
			{std::string(descriptor.key), unit_name(descriptor.unit)});
	}
	return result;
}

} // namespace

webengine::Response get_history_capabilities(
	AppContext &app, const webengine::RequestContext &)
{
	try {
		return json_response(webengine::http::status::ok,
			capabilities(app.database.historian_capabilities()));
	} catch (const std::exception &error) {
		log_api_failure("/api/v1/meter/history/capabilities", error);
		return error_response(webengine::http::status::service_unavailable,
			error.what());
	}
}

webengine::Response get_history_health(
	AppContext &app, const webengine::RequestContext &)
{
	try {
		return json_response(webengine::http::status::ok,
			app.database.historian_status());
	} catch (const std::exception &error) {
		log_api_failure("/api/v1/meter/history/health", error);
		return error_response(webengine::http::status::service_unavailable,
			error.what());
	}
}

webengine::Response post_history_query(
	AppContext &app, const webengine::RequestContext &context)
{
	try {
		HistoryQueryDto request;
		if (glz::read_json(request, context.request.body()))
			return error_response(webengine::http::status::bad_request,
				"invalid history query JSON");
		if (request.limit == 0 || request.limit > 50000)
			throw std::invalid_argument(
				"history limit must be between 1 and 50000");
		if (request.end_nanoseconds <= request.start_nanoseconds)
			throw std::invalid_argument("history time range is invalid");

		history::HistoryQuery query;
		query.period = parse_period(request.period);
		query.start_nanoseconds = request.start_nanoseconds;
		query.end_nanoseconds = request.end_nanoseconds;
		query.limit = request.limit;
		for (const auto &attribute : request.attributes)
			query.attributes.push_back(parse_attribute(attribute));
		if (query.attributes.empty())
			throw std::invalid_argument(
				"at least one history attribute is required");

		const auto points = app.database.query(query);
		HistoryResponseDto response;
		response.period = request.period;
		response.truncated = points.size() == request.limit;
		response.points.reserve(points.size());
		for (const auto &point : points) {
			response.points.push_back({point.measured_at_nanoseconds,
				point.source_sequence,
				std::string(mnc::meter::describe(
					{point.attribute, std::nullopt}).key),
				point.value, quality_name(point.quality)});
		}
		return json_response(webengine::http::status::ok, response);
	} catch (const std::invalid_argument &error) {
		return error_response(webengine::http::status::bad_request,
			error.what());
	} catch (const std::exception &error) {
		log_api_failure("/api/v1/meter/history/query", error);
		return error_response(webengine::http::status::service_unavailable,
			error.what());
	}
}

} // namespace msap1::web::api
