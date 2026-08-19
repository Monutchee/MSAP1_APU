#include "msap1/mqtt/meter_snapshot_payload_encoder.hpp"

#include "msap1/mqtt/meter_publication_catalog.hpp"

#include <glaze/glaze.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace msap1::mqtt {
namespace {

struct PayloadValue {
	std::optional<double> value;
	std::string unit;
	std::string quality;
	std::uint64_t source_sequence = 0;
};

struct MeasurementTiming {
	std::optional<std::string> measured_at;
	std::optional<std::string> window_start;
	std::optional<std::string> window_end;
};

struct SnapshotPayload {
	std::string schema = "mnc.meter.snapshot.v1";
	std::string device = "msap1";
	std::string publication;
	std::string period;
	std::uint64_t sequence = 0;
	std::uint32_t configuration_generation = 0;
	std::string published_at;
	MeasurementTiming measurement;
	std::map<std::string, PayloadValue> values;
};

std::string iso_utc(std::int64_t nanoseconds)
{
	using namespace std::chrono;
	const auto point = system_clock::time_point{
		duration_cast<system_clock::duration>(
			std::chrono::nanoseconds{nanoseconds})};
	const auto seconds_point = floor<seconds>(point);
	const auto fraction = duration_cast<milliseconds>(point - seconds_point).count();
	const auto value = system_clock::to_time_t(seconds_point);
	std::tm utc{};
	::gmtime_r(&value, &utc);
	std::ostringstream output;
	output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.'
		<< std::setw(3) << std::setfill('0') << fraction << 'Z';
	return output.str();
}

std::string now_utc()
{
	return iso_utc(std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string quality(mnc::meter::ReadingQuality quality)
{
	switch (quality) {
	case mnc::meter::ReadingQuality::Unavailable: return "unavailable";
	case mnc::meter::ReadingQuality::Valid: return "valid";
	case mnc::meter::ReadingQuality::Invalid: return "invalid";
	case mnc::meter::ReadingQuality::OutOfRange: return "out_of_range";
	case mnc::meter::ReadingQuality::TimedOut: return "timed_out";
	case mnc::meter::ReadingQuality::ArithmeticError: return "arithmetic_error";
	}
	return "unavailable";
}

std::pair<double, std::string> engineering(std::int64_t value,
	mnc::meter::MeterUnit unit)
{
	switch (unit) {
	case mnc::meter::MeterUnit::MilliHertz:
		return {static_cast<double>(value) / 1'000.0, "Hz"};
	case mnc::meter::MeterUnit::MicroVolts:
		return {static_cast<double>(value) / 1'000'000.0, "V"};
	case mnc::meter::MeterUnit::MicroAmperes:
		return {static_cast<double>(value) / 1'000'000.0, "A"};
	case mnc::meter::MeterUnit::Picowatts:
		return {static_cast<double>(value) / 1e12, "W"};
	case mnc::meter::MeterUnit::PicoVoltAmperes:
		return {static_cast<double>(value) / 1e12, "VA"};
	case mnc::meter::MeterUnit::PowerFactorMillionths:
		return {static_cast<double>(value) / 1'000'000.0, "PF"};
	case mnc::meter::MeterUnit::Picovars:
		return {static_cast<double>(value) / 1e12, "var"};
	case mnc::meter::MeterUnit::Millidegrees:
		return {static_cast<double>(value) / 1000.0, "deg"};
	}
	return {0.0, "unknown"};
}

} // namespace

std::string MeterSnapshotPayloadEncoder::encode(
	const mnc::meter::MeterSnapshot &snapshot,
	std::string_view publication_id,
	std::span<const mnc::meter::MeterAttributeKey> selected) const
{
	SnapshotPayload payload;
	payload.publication = publication_id;
	payload.period = MeterPublicationCatalog::period_id(snapshot.period);
	payload.sequence = snapshot.sequence;
	payload.configuration_generation = snapshot.configuration_generation;
	payload.published_at = now_utc();

	if (snapshot.timing && snapshot.timing->utc_start_nanoseconds) {
		const auto start = *snapshot.timing->utc_start_nanoseconds;
		payload.measurement.window_start = iso_utc(start);
		std::int64_t window = 0;
		for (const auto &value : snapshot.values)
			window = std::max(window, value.calculation_window_nanoseconds);
		if (window > 0) {
			payload.measurement.window_end = iso_utc(start + window);
			payload.measurement.measured_at = iso_utc(start + window);
		} else {
			payload.measurement.measured_at = iso_utc(start);
		}
	}

	for (const auto attribute : selected) {
		const auto descriptor = mnc::meter::describe(attribute);
		const auto found = std::ranges::find_if(snapshot.values,
			[attribute](const auto &value) {
				return value.attribute == attribute;
			});
		PayloadValue output;
		output.unit = engineering(0, descriptor.unit).second;
		if (found != snapshot.values.end()) {
			output.quality = quality(found->quality);
			output.source_sequence = found->source_sequence;
			if (found->quality == mnc::meter::ReadingQuality::Valid)
				output.value = engineering(found->value, found->unit).first;
		} else {
			output.quality = "unavailable";
		}
		payload.values.emplace(std::string(descriptor.key), std::move(output));
	}

	/* Null is part of the wire contract: an unavailable selected reading must
	 * remain present and distinguishable from a valid numeric zero. */
	const auto encoded = glz::write<glz::opts{
		.skip_null_members = false}>(payload);
	if (!encoded)
		throw std::runtime_error("cannot encode MQTT meter payload");
	return *encoded;
}

} // namespace msap1::mqtt
