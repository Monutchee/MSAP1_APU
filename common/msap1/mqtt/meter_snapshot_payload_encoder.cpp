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
#include <variant>

namespace msap1::mqtt {
namespace {

struct PayloadValue {
	std::optional<std::variant<double, std::string>> value;
	std::string unit;
	std::string quality;
	std::uint64_t source_sequence = 0;
};

struct MeasurementTiming {
	std::optional<std::string> measured_at;
	std::optional<std::string> window_start;
	std::optional<std::string> window_end;
};

struct EnergyMetadata {
	std::string session_id;
	std::string reset_epoch;
	std::string last_sample_index;
	std::string accepted_samples;
	std::string skipped_samples;
	std::uint32_t accepted_blocks = 0;
	std::uint32_t skipped_blocks = 0;
	bool incomplete_accumulation = false;
	bool saturated = false;
	bool discontinuity = false;
};

struct DemandMetadata {
	std::string session_id;
	std::string peak_reset_epoch;
	std::string last_sample_index;
	std::string interval_anchor_sample;
	std::uint32_t source_interval_count = 0;
	std::uint32_t source_status = 0;
	std::string method;
	std::uint32_t window_seconds = 0;
	std::uint32_t update_seconds = 0;
	std::uint32_t profile_generation = 0;
	bool time_aligned = false;
	bool contaminated = false;
	bool boundary_valid = false;
	bool incomplete_accumulation = false;
	bool saturated = false;
};

struct Frequency10sMetadata {
	std::string interval_end_sample_index;
	std::string utc_start_nanoseconds;
	std::string utc_end_nanoseconds;
	std::string utc_uncertainty_nanoseconds;
	std::uint32_t measured_sample_rate_millihz = 0;
	std::uint32_t source_sequence = 0;
	std::uint32_t boundary_generation = 0;
	std::uint32_t source_status = 0;
	std::uint32_t status = 0;
	std::uint32_t reasons = 0;
	std::uint32_t observer_drop_count = 0;
	std::uint8_t guard_flags = 0;
	std::uint32_t observed_crossings = 0;
	std::uint32_t included_crossings = 0;
	std::uint32_t rejected_cycles = 0;
	std::string duration_q16_samples;
	std::string first_crossing_q16_samples;
	std::string last_crossing_q16_samples;
	std::uint8_t nominal_frequency_hz = 0;
	std::uint8_t reference_channel = 0;
	std::uint8_t filter_profile = 0;
	std::uint8_t calibration_profile = 0;
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
	std::optional<EnergyMetadata> energy;
	std::optional<DemandMetadata> demand;
	std::optional<Frequency10sMetadata> frequency_10s;
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
		/* The PL publishes the 0..359.999-degree convention directly. */
		return {static_cast<double>(value) / 1000.0, "deg"};
	case mnc::meter::MeterUnit::RatioMillionths:
		/* millionths of the positive sequence -> percent. */
		return {static_cast<double>(value) / 10000.0, "%"};
	case mnc::meter::MeterUnit::MicroWattHours:
		return {0.0, "uWh"};
	case mnc::meter::MeterUnit::MicroVarHours:
		return {0.0, "uvarh"};
	case mnc::meter::MeterUnit::MicroVoltAmpereHours:
		return {0.0, "uVAh"};
	case mnc::meter::MeterUnit::MicroWatts:
		return {0.0, "uW"};
	case mnc::meter::MeterUnit::CrestTenThousandths:
		return {static_cast<double>(value) / 10'000.0, "crest"};
	case mnc::meter::MeterUnit::CategoricalCode:
		return {static_cast<double>(value), "code"};
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
	if (snapshot.energy) {
		const auto &energy = *snapshot.energy;
		payload.energy = EnergyMetadata{
			std::to_string(energy.session_id),
			std::to_string(energy.reset_epoch),
			std::to_string(energy.last_sample_index),
			std::to_string(energy.accepted_samples),
			std::to_string(energy.skipped_samples), energy.accepted_blocks,
			energy.skipped_blocks, energy.incomplete_input,
			energy.saturated, energy.discontinuity};
	}
	if (snapshot.demand) {
		const auto &demand = *snapshot.demand;
		payload.demand = DemandMetadata{
			.session_id = std::to_string(demand.session_id),
			.peak_reset_epoch = std::to_string(demand.peak_reset_epoch),
			.last_sample_index = std::to_string(demand.last_sample_index),
			.interval_anchor_sample =
				std::to_string(demand.interval_anchor_sample),
			.source_interval_count = demand.source_interval_count,
			.source_status = demand.source_status,
			.method = demand.method == 0 ? "fixed_block" : "sliding",
			.window_seconds = demand.window_seconds,
			.update_seconds = demand.update_seconds,
			.profile_generation = demand.profile_generation,
			.time_aligned = demand.time_aligned,
			.contaminated = demand.contaminated,
			.boundary_valid = demand.boundary_valid,
			.incomplete_accumulation = demand.incomplete_input,
			.saturated = demand.saturated};
	}
	if (snapshot.frequency_10s) {
		const auto &frequency = *snapshot.frequency_10s;
		payload.frequency_10s = Frequency10sMetadata{
			.interval_end_sample_index =
				std::to_string(frequency.interval_end_sample_index),
			.utc_start_nanoseconds =
				std::to_string(frequency.utc_start_nanoseconds),
			.utc_end_nanoseconds =
				std::to_string(frequency.utc_end_nanoseconds),
			.utc_uncertainty_nanoseconds =
				std::to_string(frequency.utc_uncertainty_nanoseconds),
			.measured_sample_rate_millihz =
				frequency.measured_sample_rate_millihz,
			.source_sequence = frequency.source_sequence,
			.boundary_generation = frequency.boundary_generation,
			.source_status = frequency.source_status,
			.status = frequency.status,
			.reasons = frequency.reasons,
			.observer_drop_count = frequency.observer_drop_count,
			.guard_flags = frequency.guard_flags,
			.observed_crossings = frequency.observed_crossings,
			.included_crossings = frequency.included_crossings,
			.rejected_cycles = frequency.rejected_cycles,
			.duration_q16_samples =
				std::to_string(frequency.duration_q16_samples),
			.first_crossing_q16_samples =
				std::to_string(frequency.first_crossing_q16_samples),
			.last_crossing_q16_samples =
				std::to_string(frequency.last_crossing_q16_samples),
			.nominal_frequency_hz = frequency.nominal_frequency_hz,
			.reference_channel = frequency.reference_channel,
			.filter_profile = frequency.filter_profile,
			.calibration_profile = frequency.calibration_profile,
		};
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
			if (found->quality == mnc::meter::ReadingQuality::Valid) {
				const bool exact_integer = found->unit ==
					mnc::meter::MeterUnit::MicroWattHours ||
					found->unit == mnc::meter::MeterUnit::MicroVarHours ||
					found->unit ==
						mnc::meter::MeterUnit::MicroVoltAmpereHours ||
					found->unit == mnc::meter::MeterUnit::MicroWatts;
				output.value = exact_integer
					? std::variant<double, std::string>{
						std::in_place_type<std::string>,
						std::to_string(found->value)}
					: std::variant<double, std::string>{
						std::in_place_type<double>,
						engineering(found->value, found->unit).first};
			}
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
