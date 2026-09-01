/** @file history_routes.cpp Bounded typed historian query endpoints. */

#include "response.hpp"
#include "routes.hpp"

#include "mnc/MeterDataProvider/attributes/meter_attribute.hpp"
#include "mnc/MeterDataProvider/attributes/meter_attribute_set.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <exception>
#include <optional>
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
	std::optional<std::string> after;
};

struct HistoryPointDto {
	std::int64_t measured_at_nanoseconds = 0;
	std::uint64_t source_sequence = 0;
	std::string attribute;
	std::string value;
	std::string quality;
	std::optional<std::string> reset_epoch;
};

struct HistoryResponseDto {
	std::string period;
	std::vector<HistoryPointDto> points;
	bool truncated = false;
	std::optional<std::string> next_cursor;
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

const std::vector<Attribute> historical_attributes = [] {
	std::vector<Attribute> result;
	for (const auto key : mnc::meter::defined_attributes())
		result.push_back(key.id);
	return result;
}();

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
	if (value == "demand") return MeasurementPeriod::Demand;
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
	case MeasurementPeriod::Demand: return "demand";
	case MeasurementPeriod::Min10Live:
	case MeasurementPeriod::Hour2Live:
		throw std::invalid_argument(
			"non-normative live intervals are not historian capabilities");
	}
	throw std::invalid_argument("unsupported historian period capability");
}

std::string cursor_token(const history::HistoryCursor &cursor)
{
	return std::to_string(cursor.measured_at_nanoseconds) + ":" +
		std::to_string(cursor.block_source_sequence) + ":" +
		std::to_string(cursor.record_kind) + ":" +
		std::to_string(cursor.block_id) + ":" +
		std::to_string(static_cast<std::uint16_t>(cursor.attribute));
}

template<class Integer>
Integer cursor_integer(std::string_view value)
{
	Integer result{};
	const auto [end, error] = std::from_chars(value.data(),
		value.data() + value.size(), result);
	if (error != std::errc{} || end != value.data() + value.size())
		throw std::invalid_argument("invalid history continuation cursor");
	return result;
}

history::HistoryCursor parse_cursor(std::string_view token)
{
	std::array<std::string_view, 5> fields;
	for (auto &field : fields) {
		const auto separator = token.find(':');
		field = token.substr(0, separator);
		if (field.empty())
			throw std::invalid_argument("invalid history continuation cursor");
		if (separator == std::string_view::npos) {
			token = {};
			continue;
		}
		token.remove_prefix(separator + 1u);
	}
	if (!token.empty())
		throw std::invalid_argument("invalid history continuation cursor");
	const auto attribute = cursor_integer<std::uint16_t>(fields[4]);
	if (!std::ranges::any_of(mnc::meter::defined_attributes(),
		[attribute](const auto &candidate) {
			return static_cast<std::uint16_t>(candidate.id) == attribute;
		}))
		throw std::invalid_argument("invalid history continuation cursor");
	return {
		.measured_at_nanoseconds = cursor_integer<std::int64_t>(fields[0]),
		.block_source_sequence = cursor_integer<std::uint64_t>(fields[1]),
		.record_kind = cursor_integer<std::uint32_t>(fields[2]),
		.block_id = cursor_integer<std::uint64_t>(fields[3]),
		.attribute = static_cast<Attribute>(attribute),
	};
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
			{std::string(descriptor.key),
			 std::string(mnc::meter::unit_name(descriptor.unit))});
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
		if (request.after)
			query.after = parse_cursor(*request.after);
		for (const auto &attribute : request.attributes)
			query.attributes.push_back(parse_attribute(attribute));
		if (query.attributes.empty())
			throw std::invalid_argument(
				"at least one history attribute is required");

		const auto points = app.database.query(query);
		HistoryResponseDto response;
		response.period = request.period;
		response.truncated = points.size() == request.limit;
		if (response.truncated && !points.empty())
			response.next_cursor = cursor_token(points.back().cursor);
		response.points.reserve(points.size());
		for (const auto &point : points) {
			response.points.push_back({point.measured_at_nanoseconds,
				point.source_sequence,
				std::string(mnc::meter::describe(
					{point.attribute, std::nullopt}).key),
				std::to_string(point.value), quality_name(point.quality),
				point.reset_epoch
					? std::optional<std::string>(
						std::to_string(*point.reset_epoch))
					: std::nullopt});
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
