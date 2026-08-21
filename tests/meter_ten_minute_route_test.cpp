#include "meter_dto.hpp"

#include <chrono>
#include <cstdint>
#include <stdexcept>

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

mnc::meter::MeterAttributeValue value(Id id, Unit unit,
				      std::int64_t reading,
				      Quality quality = Quality::Valid)
{
	return {Key{id, std::nullopt}, unit, quality, reading, 42, 0,
		1'920'000, 600'000'000'000LL};
}

msap1::MeterSnapshotResponse response()
{
	msap1::MeterSnapshotResponse result{};
	result.running = true;
	result.has_snapshot = true;
	result.diagnostics.sample_rate_hz = 32'000;
	result.snapshot.period = mnc::meter::MeasurementPeriod::Min10;
	result.snapshot.sequence = 42;
	result.snapshot.configuration_generation = 77;
	result.snapshot.updated_at_nanoseconds =
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
	mnc::meter::MeterSnapshotTiming timing{};
	timing.quality = mnc::meter::TimeQuality::Synchronized;
	timing.first_sample_index = 123'000;
	timing.sample_count = 19'200'000;
	timing.cycle_count = 36'000;
	timing.nominal_frequency_hz = 60;
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
	};
	return result;
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
		projected->time_quality == "synchronized",
		"ten-minute identity or provenance was changed");
	require(projected->channels[0].valid &&
		projected->channels[0].rms == 1.0 &&
		projected->channels[4].rms == 122.0 &&
		projected->channels[6].rms == 120.0 &&
		!projected->channels[7].valid,
		"hardware channel order was not preserved");
	require(projected->attributes.size() == 2 &&
		projected->attributes[0].key == "voltage.ll.ab.rms" &&
		projected->attributes[0].value == 208.0 &&
		projected->attributes[1].key == "power.active.total" &&
		projected->attributes[1].value == 720.0,
		"derived attributes were not projected in engineering units");
}

void absence_and_malformed_period_are_distinct()
{
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
	absence_and_malformed_period_are_distinct();
	return 0;
}
