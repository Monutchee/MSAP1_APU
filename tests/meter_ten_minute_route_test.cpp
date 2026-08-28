#include "meter_dto.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include <glaze/glaze.hpp>

namespace {

using Id = mnc::meter::MeterAttributeId;
using Key = mnc::meter::MeterAttributeKey;
using Quality = mnc::meter::ReadingQuality;
using Unit = mnc::meter::MeterUnit;

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
	return {Key{id, std::nullopt}, unit, quality, reading, 42, 0,
		1'920'000, 600'000'000'000LL};
}

msap1::MeterSnapshotResponse response(
	mnc::meter::MeasurementPeriod period =
		mnc::meter::MeasurementPeriod::Min10,
	std::uint64_t sample_count = 19'200'000,
	std::uint32_t cycle_count = 36'000)
{
	msap1::MeterSnapshotResponse result{};
	result.running = true;
	result.has_snapshot = true;
	result.diagnostics.sample_rate_hz = 32'000;
	result.snapshot.period = period;
	result.snapshot.sequence = 42;
	result.snapshot.configuration_generation = 77;
	result.snapshot.updated_at_nanoseconds =
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
	mnc::meter::MeterSnapshotTiming timing{};
	timing.quality = mnc::meter::TimeQuality::Synchronized;
	timing.first_sample_index = 123'000;
	timing.sample_count = sample_count;
	timing.cycle_count = cycle_count;
	timing.nominal_frequency_hz = 60;
	timing.utc_start_nanoseconds = 1'788'000'200'200'000'000LL;
	timing.utc_uncertainty_nanoseconds = 500;
	result.snapshot.timing = timing;
	result.snapshot.values = {
		value(Id::VanRms, Unit::MicroVolts, 120'000'000),
		value(Id::VbnRms, Unit::MicroVolts, 121'000'000),
		value(Id::VcnRms, Unit::MicroVolts, 122'000'000),
		value(Id::IaRms, Unit::MicroAmperes, 1'000'000),
		value(Id::IbRms, Unit::MicroAmperes, 2'000'000),
		value(Id::IcRms, Unit::MicroAmperes, 3'000'000),
		value(Id::InRms, Unit::MicroAmperes, 0),
		value(Id::VabRms, Unit::MicroVolts, 208'000'000),
		value(Id::ActivePowerTotal, Unit::Picowatts, 720'000'000'000'000),
		value(Id::VoltagePhaseAngleA, Unit::Millidegrees, 0),
		value(Id::VoltagePhaseAngleB, Unit::Millidegrees, 240'000),
		value(Id::VoltagePhaseAngleC, Unit::Millidegrees, 120'000),
		value(Id::CurrentPhaseAngleA, Unit::Millidegrees, 350'000),
		value(Id::CurrentPhaseAngleB, Unit::Millidegrees, 230'000),
		value(Id::CurrentPhaseAngleC, Unit::Millidegrees, 110'000),
	};
	return result;
}

const msap1::web::api::MeterAttributeDto *attribute(
	const msap1::web::api::MeterTenMinuteDto &snapshot,
	std::string_view key)
{
	const auto found = std::find_if(snapshot.attributes.begin(),
		snapshot.attributes.end(), [key](const auto &candidate) {
			return candidate.key == key;
		});
	return found == snapshot.attributes.end() ? nullptr : &*found;
}

void projects_the_finalized_two_hour_snapshot()
{
	const auto projected = msap1::web::api::meter_two_hour_dto(response(
		mnc::meter::MeasurementPeriod::Hour2, 230'400'000, 432'000));
	require(projected.has_value(), "two-hour snapshot was unavailable");
	require(projected->sequence == 42 &&
		projected->record_complete &&
		projected->sample_count == 230'400'000 &&
		projected->cycle_count == 432'000 &&
		projected->channels[6].valid &&
		projected->channels[6].rms == 120.0,
		"two-hour identity, provenance, or channel projection changed");
	const auto *voltage_b = attribute(*projected,
		"phase.angle.voltage.b");
	const auto *current_c = attribute(*projected,
		"phase.angle.current.c");
	require(voltage_b && voltage_b->valid && voltage_b->value == 240.0 &&
		current_c && current_c->valid && current_c->value == 110.0,
		"two-hour phase angles were not projected");
}

void projects_the_finalized_ten_minute_snapshot()
{
	const auto projected = msap1::web::api::meter_ten_minute_dto(response());
	require(projected.has_value(), "ten-minute snapshot was unavailable");
	require(projected->sequence == 42 &&
		projected->configuration_generation == 77 &&
		projected->sample_rate_hz == 32'000 &&
		projected->sample_count == 19'200'000 &&
		projected->cycle_count == 36'000 &&
		projected->nominal_frequency_hz == 60 &&
		projected->time_quality == "synchronized" &&
		projected->utc_start_nanoseconds ==
			1'788'000'200'200'000'000LL &&
		projected->utc_uncertainty_nanoseconds == 500,
		"ten-minute identity or provenance was changed");
	require(projected->channels[0].valid &&
		projected->channels[0].rms == 1.0 &&
		projected->channels[4].rms == 122.0 &&
		projected->channels[6].rms == 120.0 &&
		!projected->channels[7].valid,
		"hardware channel order was not preserved");
	require(projected->attributes.size() == 8 &&
		projected->attributes[0].key == "voltage.ll.ab.rms" &&
		projected->attributes[0].value == 208.0 &&
		projected->attributes[1].key == "power.active.total" &&
		projected->attributes[1].value == 720.0 &&
		projected->attributes[1].quality == "valid" &&
		projected->attributes[1].source_sequence == 42 &&
		attribute(*projected, "phase.angle.voltage.b") &&
		attribute(*projected, "phase.angle.voltage.b")->valid &&
		attribute(*projected, "phase.angle.voltage.b")->value == 240.0 &&
		attribute(*projected, "phase.angle.current.c") &&
		attribute(*projected, "phase.angle.current.c")->valid &&
		attribute(*projected, "phase.angle.current.c")->value == 110.0,
		"derived attributes were not projected in engineering units");

	auto partial = response();
	partial.snapshot.values.back().source_sequence = 41;
	const auto incomplete = msap1::web::api::meter_ten_minute_dto(partial);
	require(incomplete && !incomplete->record_complete,
		"a mixed-sequence ten-minute family was marked complete");
}

void absence_and_malformed_period_are_distinct()
{
	require(json(msap1::web::api::MeterTenMinuteUnavailableDto{}) ==
			R"({"available":false})" &&
		json(msap1::web::api::MeterTwoHourUnavailableDto{}) ==
			R"({"available":false})",
		"long-interval pending response shape changed");

	auto missing = response();
	missing.has_snapshot = false;
	require(!msap1::web::api::meter_ten_minute_dto(missing),
		"missing ten-minute result was rendered as available");

	auto wrong = response();
	wrong.snapshot.period = mnc::meter::MeasurementPeriod::Basic;
	bool rejected = false;
	try {
		(void)msap1::web::api::meter_ten_minute_dto(wrong);
	} catch (const std::invalid_argument &) {
		rejected = true;
	}
	require(rejected, "wrong snapshot period was accepted");
}

} // namespace

int main()
{
	projects_the_finalized_ten_minute_snapshot();
	projects_the_finalized_two_hour_snapshot();
	absence_and_malformed_period_are_distinct();
	return 0;
}
