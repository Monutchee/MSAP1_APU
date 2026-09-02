/** @file attribute_routes.cpp Canonical snapshot/historian attribute API. */

#include "query.hpp"
#include "openapi.hpp"
#include "response.hpp"
#include "routes.hpp"

#include "mnc/MeterDataProvider/attributes/meter_attribute.hpp"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace msap1::web::api {
namespace {

using namespace mnc::meter;

struct AttributeDescriptorDto {
	std::string id;
	std::string label;
	std::string group;
	std::string unit;
	std::string value_kind;
	std::vector<std::string> search_aliases;
	std::vector<std::string> calculations;
	std::vector<std::string> periods;
};

struct AttributePeriodDto {
	std::string id;
	std::string label;
	std::vector<std::string> attributes;
};

struct AttributeCatalogDto {
	std::string usage;
	std::vector<AttributePeriodDto> periods;
	std::vector<AttributeDescriptorDto> attributes;
};

std::string group_name(MeterAttributeGroup group)
{
	switch (group) {
	case MeterAttributeGroup::Frequency: return "frequency";
	case MeterAttributeGroup::VoltageLnRms: return "voltage_ln_rms";
	case MeterAttributeGroup::VoltageLlRms: return "voltage_ll_rms";
	case MeterAttributeGroup::CurrentRms: return "current_rms";
	case MeterAttributeGroup::Fundamental: return "fundamental";
	case MeterAttributeGroup::ActivePower: return "active_power";
	case MeterAttributeGroup::ApparentPower: return "apparent_power";
	case MeterAttributeGroup::PowerFactor: return "power_factor";
	case MeterAttributeGroup::ReactivePower: return "reactive_power";
	case MeterAttributeGroup::DisplacementPowerFactor:
		return "displacement_power_factor";
	case MeterAttributeGroup::PhaseAngle: return "phase_angle";
	case MeterAttributeGroup::Unbalance: return "unbalance";
	case MeterAttributeGroup::SequenceComponents:
		return "sequence_components";
	case MeterAttributeGroup::Energy: return "energy";
	case MeterAttributeGroup::Demand: return "demand";
	case MeterAttributeGroup::CrestFactor: return "crest_factor";
	case MeterAttributeGroup::LoadNature: return "load_nature";
	case MeterAttributeGroup::AllDefined: return "other";
	}
	return "other";
}

std::string value_kind_name(MeterAttributeValueKind kind)
{
	switch (kind) {
	case MeterAttributeValueKind::Linear: return "linear";
	case MeterAttributeValueKind::CircularAngle: return "circular_angle";
	case MeterAttributeValueKind::CumulativeCounter:
		return "cumulative_counter";
	case MeterAttributeValueKind::Peak: return "peak";
	case MeterAttributeValueKind::Categorical: return "categorical";
	}
	return "linear";
}

std::string calculation_name(MeterAttributeCalculation calculation)
{
	switch (calculation) {
	case MeterAttributeCalculation::Minimum: return "minimum";
	case MeterAttributeCalculation::Maximum: return "maximum";
	case MeterAttributeCalculation::Average: return "average";
	case MeterAttributeCalculation::Last: return "last";
	case MeterAttributeCalculation::CircularAverage:
		return "circular_average";
	case MeterAttributeCalculation::First: return "first";
	case MeterAttributeCalculation::Delta: return "delta";
	}
	return "last";
}

AttributeCatalogDto catalog(MeterAttributeUsage usage)
{
	AttributeCatalogDto result;
	result.usage = usage == MeterAttributeUsage::Snapshot
		? "snapshot" : "historian";
	for (const auto &period : defined_measurement_periods()) {
		if ((usage == MeterAttributeUsage::Snapshot && !period.snapshot) ||
		    (usage == MeterAttributeUsage::Historian && !period.historian))
			continue;
		AttributePeriodDto item{std::string(period.key),
			std::string(period.label), {}};
		for (const auto attribute : attributes_for(period.period, usage))
			item.attributes.emplace_back(describe(attribute).key);
		result.periods.push_back(std::move(item));
	}

	for (const auto attribute : defined_attributes()) {
		const auto descriptor = describe(attribute);
		AttributeDescriptorDto item{
			std::string(descriptor.key), std::string(descriptor.label),
			group_name(descriptor.group), std::string(unit_name(descriptor.unit)),
			value_kind_name(descriptor.value_kind), {}, {}, {}};
		for (const auto alias : descriptor.search_aliases)
			item.search_aliases.emplace_back(alias);
		for (const auto calculation : descriptor.calculations)
			item.calculations.push_back(calculation_name(calculation));
		for (const auto &period : result.periods)
			if (std::ranges::find(period.attributes, item.id) !=
			    period.attributes.end())
				item.periods.push_back(period.id);
		if (!item.periods.empty())
			result.attributes.push_back(std::move(item));
	}
	return result;
}

} // namespace

webengine::Response get_meter_attributes(
	AppContext &, const webengine::RequestContext &context)
{
	try {
		const auto target = context.request.target();
		const auto parameters = query_parameters(
			std::string_view(target.data(), target.size()));
		for (const auto &[name, value] : parameters) {
			(void)value;
			if (name != "usage")
				throw std::invalid_argument(
					"unsupported attribute query parameter: " + name);
		}
		const auto found = parameters.find("usage");
		if (found == parameters.end())
			throw std::invalid_argument(
				"usage must be snapshot or historian");
		MeterAttributeUsage usage;
		if (found->second == "snapshot")
			usage = MeterAttributeUsage::Snapshot;
		else if (found->second == "historian")
			usage = MeterAttributeUsage::Historian;
		else
			throw std::invalid_argument(
				"usage must be snapshot or historian");
		return json_response(webengine::http::status::ok, catalog(usage));
	} catch (const std::invalid_argument &error) {
		return error_response(webengine::http::status::bad_request,
			error.what());
	} catch (const std::exception &error) {
		log_api_failure("/api/v1/meter/attributes", error);
		return error_response(webengine::http::status::service_unavailable,
			error.what());
	}
}

void document_attribute_routes(DocumentedApiRegistry &registry)
{
	using V = webengine::http::verb;
	constexpr auto path = "/api/v1/meter/attributes";
	registry.add_query_parameter(V::get, path, "usage", "string", true,
		"Capability view to return", {"snapshot", "historian"},
		"snapshot");
	registry.add_json_response<AttributeCatalogDto>(V::get, path, 200,
		"MeterAttributeCatalog", "Canonical period-aware attribute catalog");
	registry.add_error_response(V::get, path, 400,
		"The usage query is absent or invalid");
	registry.add_error_response(V::get, path, 503,
		"The attribute catalogue is unavailable");
}

} // namespace msap1::web::api
