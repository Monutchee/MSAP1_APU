#include "meter_dto.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <glaze/glaze.hpp>

/*
 * GET /api/v1/meter/aggregate contract tests.
 *
 * The endpoint uses one typed Cycles150_180 snapshot so its RMS channels,
 * POWER/PHASOR/UNBAL attributes, timing, and informative frequency all refer
 * to the same provider view. The projection remains WebEngine-free and is
 * pinned here without linking the HTTP stack.
 */

namespace {

using Id = mnc::meter::MeterAttributeId;
using Key = mnc::meter::MeterAttributeKey;
using Quality = mnc::meter::ReadingQuality;
using Unit = mnc::meter::MeterUnit;
using msap1::web::api::meter_aggregate_dto;
using msap1::web::api::meter_aggregate_snapshot_selection;
using msap1::web::api::MeterAggregateUnavailableDto;

void require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

std::string json(const auto &value)
{
	auto text = glz::write_json(value);
	if (!text)
		throw std::runtime_error("failed to serialize the DTO");
	return *text;
}

mnc::meter::MeterAttributeValue value(Id id, Unit unit,
				      std::int64_t reading,
				      Quality quality = Quality::Valid)
{
	return {Key{id, std::nullopt}, unit, quality, reading, 7, 0,
		384'015, 3'000'117'187LL};
}

msap1::MeterSnapshotResponse contract_response()
{
	msap1::MeterSnapshotResponse response{};
	response.running = true;
	response.has_snapshot = true;
	response.snapshot.period = mnc::meter::MeasurementPeriod::Cycles150_180;
	response.snapshot.sequence = 7;
	response.snapshot.configuration_generation = 3'545'159'487u;
	response.snapshot.updated_at_nanoseconds =
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::system_clock::now().time_since_epoch() -
			std::chrono::milliseconds(1200)).count();
	mnc::meter::MeterSnapshotTiming timing{};
	timing.quality = mnc::meter::TimeQuality::Synchronized;
	timing.first_sample_index = 331'990'790u;
	timing.sample_count = 384'015;
	timing.sample_rate_hz = 128'000;
	timing.cycle_count = 180;
	timing.nominal_frequency_hz = 60;
	timing.source_interval_count = 15;
	timing.first_source_sequence = 100;
	timing.last_source_sequence = 114;
	timing.utc_start_nanoseconds = 1'788'000'200'200'000'000LL;
	timing.utc_uncertainty_nanoseconds = 250;
	response.snapshot.timing = timing;
	response.snapshot.values = {
		/* Informative only: the typed value deliberately remains unavailable. */
		value(Id::Frequency, Unit::MilliHertz, 60'000,
		      Quality::Unavailable),
		value(Id::VanRms, Unit::MicroVolts, 120'000'000),
		value(Id::VbnRms, Unit::MicroVolts, 120'000'000),
		value(Id::VcnRms, Unit::MicroVolts, 120'000'000),
		value(Id::IaRms, Unit::MicroAmperes, 1'500'000),
		value(Id::IbRms, Unit::MicroAmperes, 1'500'000),
		value(Id::IcRms, Unit::MicroAmperes, 1'500'000),
		value(Id::InRms, Unit::MicroAmperes, 1'500'000),
		value(Id::VabRms, Unit::MicroVolts, 208'000'000),
		value(Id::ActivePowerTotal, Unit::Picowatts,
		      720'000'000'000'000),
		value(Id::VoltagePhaseAngleA, Unit::Millidegrees, 0),
		value(Id::VoltagePhaseAngleB, Unit::Millidegrees, 240'000),
		value(Id::VoltagePhaseAngleC, Unit::Millidegrees, 120'000),
		value(Id::CurrentPhaseAngleA, Unit::Millidegrees, 350'000),
		value(Id::CurrentPhaseAngleB, Unit::Millidegrees, 230'000),
		value(Id::CurrentPhaseAngleC, Unit::Millidegrees, 110'000),
	};
	return response;
}

void selection_requests_the_complete_catalog()
{
	const auto selection = meter_aggregate_snapshot_selection();
	const auto defined = mnc::meter::defined_attributes();
	require(selection.period ==
			mnc::meter::MeasurementPeriod::Cycles150_180,
		"the endpoint selected the wrong period");
	require(selection.attributes.size() == defined.size(),
		"the endpoint did not request the complete scalar catalog");
	const auto contains = [&selection](Id id) {
		return std::ranges::any_of(selection.attributes,
			[id](const auto &attribute) {
				return attribute.id == id && !attribute.index;
			});
	};
	require(contains(Id::Frequency) && contains(Id::ActivePowerTotal) &&
		contains(Id::VoltagePhaseAngleA) &&
		contains(Id::CurrentPhaseAngleC) &&
		contains(Id::VoltageUnbalance),
		"the aggregate selection omitted a required attribute family");
}

void absence_renders_the_unavailable_shape()
{
	auto response = contract_response();
	response.has_snapshot = false;
	require(!meter_aggregate_dto(response).has_value(),
		"a missing aggregate was rendered as available");

	response.has_snapshot = true;
	response.running = false;
	require(!meter_aggregate_dto(response).has_value(),
		"a stopped pipeline was rendered as available");
	require(json(MeterAggregateUnavailableDto{}) ==
			R"({"available":false})",
		"the unavailable body is not exactly {\"available\":false}");
}

void a_typed_aggregate_exposes_derived_attributes()
{
	const auto aggregate = meter_aggregate_dto(contract_response());
	require(aggregate.has_value(), "the aggregate was not rendered");
	require(aggregate->available && aggregate->sequence == 7 &&
		aggregate->record_complete &&
		aggregate->configuration_generation == 3'545'159'487u &&
		aggregate->sample_rate_hz == 128'000 &&
		aggregate->sample_count == 384'015 &&
		aggregate->first_sample_index == 331'990'790u &&
		aggregate->first_basic_sequence == 100 &&
		aggregate->last_basic_sequence == 114 &&
		aggregate->basic_block_count == 15 &&
		aggregate->cycle_count == 180 &&
		aggregate->nominal_frequency_hz == 60 &&
		!aggregate->arithmetic_error &&
		aggregate->time_quality == "synchronized" &&
		aggregate->utc_start_nanoseconds ==
			1'788'000'200'200'000'000LL &&
		aggregate->utc_uncertainty_nanoseconds == 250,
		"the aggregate identity or provenance changed");

	require(aggregate->channels[0].name == "ILA" &&
		aggregate->channels[0].unit == "A" &&
		aggregate->channels[0].valid &&
		aggregate->channels[0].rms == 1.5 &&
		aggregate->channels[6].name == "VLA" &&
		aggregate->channels[6].unit == "V" &&
		aggregate->channels[6].valid &&
		aggregate->channels[6].rms == 120.0 &&
		!aggregate->channels[7].valid,
		"aggregate channels were not projected in hardware order");
	require(aggregate->frequency.millihz == 60'000 &&
		aggregate->frequency.informative,
		"the informative frequency was not preserved");

	const auto attribute = [&aggregate](std::string_view key) {
		return std::find_if(aggregate->attributes.begin(),
			aggregate->attributes.end(), [&key](const auto &candidate) {
				return candidate.key == key;
			});
	};
	const auto power = attribute("power.active.total");
	const auto voltage_b = attribute("phase.angle.voltage.b");
	const auto current_c = attribute("phase.angle.current.c");
	require(power != aggregate->attributes.end() && power->valid &&
		power->unit == "W" && power->value == 720.0 &&
		power->quality == "valid" && power->source_sequence == 7,
		"aggregate power was not exposed");
	require(voltage_b != aggregate->attributes.end() && voltage_b->valid &&
		voltage_b->unit == "deg" && voltage_b->value == 240.0 &&
		current_c != aggregate->attributes.end() && current_c->valid &&
		current_c->value == 110.0,
		"aggregate phase angles were not exposed");

	const auto body = json(*aggregate);
	require(body.find(R"("attributes":[)") != std::string::npos &&
		body.find(R"("key":"phase.angle.voltage.b","unit":"deg","valid":true,"value":240)") !=
			std::string::npos &&
		body.find(R"("frequency":{"millihz":60000,"informative":true})") !=
			std::string::npos,
		"the aggregate JSON does not expose the pinned derived contract");
}

void exact_quality_and_family_completeness_are_exposed()
{
	using msap1::web::api::attribute_dto;
	const std::array qualities{
		std::pair{Quality::Valid, std::string_view{"valid"}},
		std::pair{Quality::Unavailable, std::string_view{"unavailable"}},
		std::pair{Quality::Invalid, std::string_view{"invalid"}},
		std::pair{Quality::OutOfRange, std::string_view{"out_of_range"}},
		std::pair{Quality::TimedOut, std::string_view{"timed_out"}},
		std::pair{Quality::ArithmeticError,
			std::string_view{"arithmetic_error"}},
	};
	for (const auto &[quality, expected] : qualities) {
		const auto dto = attribute_dto(value(
			Id::ActivePowerTotal, Unit::Picowatts, 1, quality));
		require(dto.quality == expected && dto.source_sequence == 7 &&
			dto.valid == (quality == Quality::Valid),
			"attribute quality or compatibility validity changed");
	}

	auto partial = contract_response();
	partial.snapshot.values.back().source_sequence = 6;
	const auto projected = meter_aggregate_dto(partial);
	require(projected && !projected->record_complete,
		"a mixed-sequence aggregate was marked complete");
	partial.snapshot.values.back().source_sequence = partial.snapshot.sequence;
	const auto completed = meter_aggregate_dto(partial);
	require(completed && completed->record_complete,
		"a converged sibling family did not become complete");

	auto unsynchronized = contract_response();
	unsynchronized.snapshot.timing->quality =
		mnc::meter::TimeQuality::Unsynchronized;
	unsynchronized.snapshot.timing->utc_start_nanoseconds.reset();
	unsynchronized.snapshot.timing->utc_uncertainty_nanoseconds.reset();
	const auto without_utc = meter_aggregate_dto(unsynchronized);
	require(without_utc && without_utc->time_quality == "unsynchronized" &&
		!without_utc->utc_start_nanoseconds &&
		!without_utc->utc_uncertainty_nanoseconds,
		"unsynchronized measurement UTC was invented");
}

void measurement_quality_and_malformed_snapshots_are_preserved()
{
	auto arithmetic = contract_response();
	for (auto &reading : arithmetic.snapshot.values) {
		if (reading.attribute.id != Id::Frequency)
			reading.quality = Quality::ArithmeticError;
	}
	const auto aggregate = meter_aggregate_dto(arithmetic);
	require(aggregate.has_value() && aggregate->arithmetic_error,
		"the aggregate arithmetic error was not projected");
	for (const auto &channel : aggregate->channels)
		require(!channel.valid && channel.rms == 0.0,
			"an arithmetic-error channel was published as valid");

	auto wrong_period = contract_response();
	wrong_period.snapshot.period = mnc::meter::MeasurementPeriod::Basic;
	bool rejected = false;
	try {
		(void)meter_aggregate_dto(wrong_period);
	} catch (const std::invalid_argument &) {
		rejected = true;
	}
	require(rejected, "a snapshot from the wrong period was accepted");

	auto incomplete = contract_response();
	incomplete.snapshot.timing->source_interval_count.reset();
	rejected = false;
	try {
		(void)meter_aggregate_dto(incomplete);
	} catch (const std::invalid_argument &) {
		rejected = true;
	}
	require(rejected, "incomplete aggregate provenance was accepted");
}

} // namespace

int main()
{
	try {
		selection_requests_the_complete_catalog();
		absence_renders_the_unavailable_shape();
		a_typed_aggregate_exposes_derived_attributes();
		exact_quality_and_family_completeness_are_exposed();
		measurement_quality_and_malformed_snapshots_are_preserved();
	} catch (const std::exception &error) {
		std::cerr << "meter aggregate route test failed: "
			  << error.what() << '\n';
		return 1;
	}
	return 0;
}
