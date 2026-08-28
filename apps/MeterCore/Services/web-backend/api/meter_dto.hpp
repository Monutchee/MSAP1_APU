#pragma once

/**
 * @file meter_dto.hpp
 * @brief Meter channel identity shared by every metering endpoint, plus the
 *        finalized aggregate transfer objects and their projections.
 *
 * The channel naming, units, and micro-unit scaling live here because
 * GET /api/v1/meter/readings, GET /api/v1/meter/aggregate, and
 * GET /api/v1/meter/minutes-10 must present the same channels and engineering
 * units the same way; duplicating the table would let the documents drift.
 *
 * The aggregate projection is deliberately free of WebEngine: it maps one
 * typed acquisition snapshot onto the response DTO and nothing else, so the
 * pinned JSON contract is directly testable without an HTTP stack.
 */

#include "msap1/acquisition/ipc/acquisition_ipc.hpp"
#include "msap1/meter/meter_data.hpp"
#include "msap1/meter/meter_record.hpp"
#include "msap1/meter/meter_timing.hpp"

#include <array>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace msap1::web::api {

/**
 * Hardware channel order of every MSAP1 meter record (MTR1 and MTR2 alike):
 * three line currents, neutral current, three line-neutral voltages in
 * reverse phase order, and the common-mode debug channel.
 */
inline constexpr std::array<const char *, msap1::meter_channel_count>
	meter_channel_names{"ILA", "ILB", "ILC", "ILN",
			    "VLC", "VLB", "VLA", "VCM"};

/** Engineering unit of one hardware channel: indices 4..6 are the voltages. */
[[nodiscard]] inline constexpr const char *meter_channel_unit(std::size_t index)
{
	return index >= 4 && index <= 6 ? "V" : "A";
}

/** PL RMS values are signed micro-units; the API publishes base units. */
[[nodiscard]] inline constexpr double meter_units(std::int64_t micro_units)
{
	return static_cast<double>(micro_units) / 1000000.0;
}

/** One catalog attribute in a meter snapshot, expressed in base units. */
struct MeterAttributeDto {
	std::string key;
	std::string unit;
	bool valid;
	double value;
};

/** Convert a strongly typed provider reading to its public engineering unit. */
[[nodiscard]] inline MeterAttributeDto attribute_dto(
	const mnc::meter::MeterAttributeValue &reading)
{
	const auto descriptor = mnc::meter::describe(reading.attribute);
	double value = 0.0;
	const char *unit = "";
	switch (reading.unit) {
	case mnc::meter::MeterUnit::MilliHertz:
		value = static_cast<double>(reading.value) / 1e3;
		unit = "Hz";
		break;
	case mnc::meter::MeterUnit::MicroVolts:
		value = static_cast<double>(reading.value) / 1e6;
		unit = "V";
		break;
	case mnc::meter::MeterUnit::MicroAmperes:
		value = static_cast<double>(reading.value) / 1e6;
		unit = "A";
		break;
	case mnc::meter::MeterUnit::Picowatts:
		value = static_cast<double>(reading.value) / 1e12;
		unit = "W";
		break;
	case mnc::meter::MeterUnit::PicoVoltAmperes:
		value = static_cast<double>(reading.value) / 1e12;
		unit = "VA";
		break;
	case mnc::meter::MeterUnit::PowerFactorMillionths:
		value = static_cast<double>(reading.value) / 1e6;
		unit = "PF";
		break;
	case mnc::meter::MeterUnit::Picovars:
		value = static_cast<double>(reading.value) / 1e12;
		unit = "var";
		break;
	case mnc::meter::MeterUnit::Millidegrees:
		value = static_cast<double>(reading.value) / 1000.0;
		unit = "deg";
		break;
	case mnc::meter::MeterUnit::RatioMillionths:
		value = static_cast<double>(reading.value) / 10000.0;
		unit = "%";
		break;
	}
	return {std::string(descriptor.key), unit,
		reading.quality == mnc::meter::ReadingQuality::Valid, value};
}

/** JSON name for the acquisition daemon's measurement time quality. */
[[nodiscard]] inline const char *
time_quality_name(msap1::meter::TimeQuality quality)
{
	switch (quality) {
	case msap1::meter::TimeQuality::Synchronized:
		return "synchronized";
	case msap1::meter::TimeQuality::Holdover:
		return "holdover";
	case msap1::meter::TimeQuality::Unsynchronized:
		break;
	}
	return "unsynchronized";
}

/** JSON name for generic snapshot timing provenance. */
[[nodiscard]] inline const char *
time_quality_name(mnc::meter::TimeQuality quality)
{
	switch (quality) {
	case mnc::meter::TimeQuality::Synchronized:
		return "synchronized";
	case mnc::meter::TimeQuality::Holdover:
		return "holdover";
	case mnc::meter::TimeQuality::Unsynchronized:
		break;
	}
	return "unsynchronized";
}

/** One channel of GET /api/v1/meter/aggregate. */
struct MeterAggregateChannelDto {
	std::uint32_t index;
	std::string name;
	std::string unit;
	bool valid;
	double rms;
};

/**
 * Aggregate frequency, INFORMATIVE ONLY.
 *
 * IEC 61000-4-30:2025 defines the standardized frequency product over its
 * own (10 s) interval, which is not implemented; the decoder therefore
 * publishes the Cycles150_180 frequency reading with quality `unavailable`.
 * This object deliberately carries no `valid` flag so no consumer can
 * mistake the mean of the 15 basic estimates for a Class A measurement.
 */
struct MeterAggregateFrequencyDto {
	std::uint32_t millihz;
	bool informative;
};

/** Body of GET /api/v1/meter/aggregate when an aggregate exists. */
struct MeterAggregateDto {
	bool available;
	std::uint64_t sequence;
	std::uint32_t configuration_generation;
	std::uint32_t sample_rate_hz;
	std::uint32_t sample_count;
	std::uint64_t first_sample_index;
	std::uint32_t first_basic_sequence;
	std::uint32_t last_basic_sequence;
	std::uint32_t basic_block_count;
	std::uint32_t cycle_count;
	std::uint32_t nominal_frequency_hz;
	bool arithmetic_error;
	/* Provenance of THIS measurement: the UTC synchronization state that
	 * applied when the aggregate was ingested, never the daemon's state
	 * at request time. "unsynchronized" | "synchronized" | "holdover". */
	std::string time_quality;
	std::uint32_t age_ms;
	std::array<MeterAggregateChannelDto, msap1::meter_channel_count>
		channels;
	std::vector<MeterAttributeDto> attributes;
	MeterAggregateFrequencyDto frequency;
};

/**
 * Body of GET /api/v1/meter/aggregate before the first aggregate exists.
 *
 * The first ~3 s after a start — and any stretch where basic blocks were
 * ineligible — legitimately has no 150/180-cycle result. That is not an
 * error, so the endpoint stays 200 and says so with this one field.
 */
struct MeterAggregateUnavailableDto {
	bool available = false;
};

/** Body of GET /api/v1/meter/minutes-10 when a finalized block exists. */
struct MeterTenMinuteDto {
	bool available;
	std::uint64_t sequence;
	std::uint32_t configuration_generation;
	std::uint32_t sample_rate_hz;
	std::uint32_t sample_count;
	std::uint64_t first_sample_index;
	std::uint32_t cycle_count;
	std::uint32_t nominal_frequency_hz;
	bool arithmetic_error;
	std::string time_quality;
	std::uint32_t age_ms;
	std::array<MeterAggregateChannelDto, msap1::meter_channel_count> channels;
	std::vector<MeterAttributeDto> attributes;
	/* Live-partial records use the same electrical-value shape, but these
	 * fields make their non-normative status and unfinished boundary
	 * provenance impossible to confuse with a completed result. */
	bool open_interval = false;
	bool non_normative = false;
	std::uint32_t source_interval_count = 0;
	std::uint64_t first_source_sequence = 0;
	std::uint64_t last_source_sequence = 0;
	std::optional<std::uint64_t> expected_end_sample_index;
	std::optional<std::uint32_t> overshoot_samples;
	std::uint64_t elapsed_milliseconds = 0;
	bool time_aligned = false;
	bool contaminated = false;
	bool boundary_valid = false;
};

/** No aligned ten-minute interval has closed since acquisition started. */
struct MeterTenMinuteUnavailableDto {
	bool available = false;
};

/* The finalized two-hour tier intentionally uses the same public shape as
 * the ten-minute tier. Both are typed snapshots with aggregate timing; only
 * their period identity and cadence differ. Keeping one schema lets clients
 * render future long intervals without inventing wire-only fields. */
using MeterTwoHourDto = MeterTenMinuteDto;
using MeterTwoHourUnavailableDto = MeterTenMinuteUnavailableDto;

[[nodiscard]] inline std::optional<MeterTenMinuteDto>
meter_long_interval_dto(const msap1::MeterSnapshotResponse &response,
	mnc::meter::MeasurementPeriod expected_period,
	std::string_view interval_name)
{
	using Id = mnc::meter::MeterAttributeId;
	using Quality = mnc::meter::ReadingQuality;
	if (!response.running || !response.has_snapshot)
		return std::nullopt;
	const auto &snapshot = response.snapshot;
	if (snapshot.period != expected_period)
		throw std::invalid_argument("cached meter snapshot is not a " +
			std::string(interval_name) + " aggregate");
	if (!snapshot.timing || !snapshot.timing->first_sample_index ||
	    !snapshot.timing->sample_count || !snapshot.timing->cycle_count ||
	    !snapshot.timing->nominal_frequency_hz)
		throw std::invalid_argument(std::string(interval_name) +
			" aggregate has incomplete timing provenance");

	const auto find = [&snapshot](Id id) -> const mnc::meter::MeterAttributeValue * {
		const auto it = std::find_if(snapshot.values.begin(), snapshot.values.end(),
			[id](const auto &value) {
				return value.attribute.id == id && !value.attribute.index;
			});
		return it == snapshot.values.end() ? nullptr : &*it;
	};
	const std::array<Id, msap1::meter_channel_count> channel_ids{
		Id::IaRms, Id::IbRms, Id::IcRms, Id::InRms,
		Id::VcnRms, Id::VbnRms, Id::VanRms, Id::Frequency,
	};

	const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
	const auto age_ns = std::max<std::int64_t>(0, now - snapshot.updated_at_nanoseconds);
	const auto age_ms64 = age_ns / 1'000'000;
	MeterTenMinuteDto result{
		true, snapshot.sequence, snapshot.configuration_generation,
		response.diagnostics.sample_rate_hz, *snapshot.timing->sample_count,
		*snapshot.timing->first_sample_index, *snapshot.timing->cycle_count,
		*snapshot.timing->nominal_frequency_hz, false,
		time_quality_name(snapshot.timing->quality),
		static_cast<std::uint32_t>(std::min<std::int64_t>(
			age_ms64, std::numeric_limits<std::uint32_t>::max())), {}, {},
		false, false, 0, 0, 0, std::nullopt, std::nullopt, 0,
		false, false, false,
	};
	result.open_interval = expected_period ==
		mnc::meter::MeasurementPeriod::Min10Live ||
		expected_period == mnc::meter::MeasurementPeriod::Hour2Live;
	result.non_normative = result.open_interval;
	result.source_interval_count =
		snapshot.timing->source_interval_count.value_or(0u);
	result.first_source_sequence =
		snapshot.timing->first_source_sequence.value_or(0u);
	result.last_source_sequence =
		snapshot.timing->last_source_sequence.value_or(0u);
	result.expected_end_sample_index =
		snapshot.timing->expected_end_sample_index;
	result.overshoot_samples = snapshot.timing->overshoot_samples;
	result.time_aligned = snapshot.timing->time_aligned.value_or(false);
	result.contaminated = snapshot.timing->contaminated.value_or(false);
	result.boundary_valid = snapshot.timing->boundary_valid.value_or(false);
	if (snapshot.timing->sample_rate_hz.value_or(0u) != 0u) {
		result.sample_rate_hz = *snapshot.timing->sample_rate_hz;
		result.elapsed_milliseconds =
			static_cast<std::uint64_t>(*snapshot.timing->sample_count) *
			1000u / *snapshot.timing->sample_rate_hz;
	}

	for (std::size_t index = 0; index < result.channels.size(); ++index) {
		const auto *reading = index == 7 ? nullptr : find(channel_ids[index]);
		const bool valid = reading && reading->quality == Quality::Valid;
		result.channels[index] = {
			static_cast<std::uint32_t>(index), meter_channel_names[index],
			meter_channel_unit(index), valid,
			valid ? meter_units(reading->value) : 0.0,
		};
	}
	for (const auto &reading : snapshot.values) {
		if (reading.quality == Quality::ArithmeticError)
			result.arithmetic_error = true;
		switch (reading.attribute.id) {
		case Id::Frequency:
		case Id::VanRms:
		case Id::VbnRms:
		case Id::VcnRms:
		case Id::IaRms:
		case Id::IbRms:
		case Id::IcRms:
		case Id::InRms:
			break;
		default:
			result.attributes.push_back(attribute_dto(reading));
			break;
		}
	}
	return result;
}

/** Project the typed Min10 provider view without recomputing meter values. */
[[nodiscard]] inline std::optional<MeterTenMinuteDto>
meter_ten_minute_dto(const msap1::MeterSnapshotResponse &response)
{
	return meter_long_interval_dto(response,
		mnc::meter::MeasurementPeriod::Min10, "ten-minute");
}

/** Project the typed Hour2 provider view without recomputing meter values. */
[[nodiscard]] inline std::optional<MeterTwoHourDto>
meter_two_hour_dto(const msap1::MeterSnapshotResponse &response)
{
	return meter_long_interval_dto(response,
		mnc::meter::MeasurementPeriod::Hour2, "two-hour");
}

/** Project the latest non-normative open ten-minute preview. */
[[nodiscard]] inline std::optional<MeterTenMinuteDto>
meter_ten_minute_live_dto(const msap1::MeterSnapshotResponse &response)
{
	return meter_long_interval_dto(response,
		mnc::meter::MeasurementPeriod::Min10Live, "live ten-minute");
}

/** Project the latest non-normative open two-hour preview. */
[[nodiscard]] inline std::optional<MeterTwoHourDto>
meter_two_hour_live_dto(const msap1::MeterSnapshotResponse &response)
{
	return meter_long_interval_dto(response,
		mnc::meter::MeasurementPeriod::Hour2Live, "live two-hour");
}

/**
 * Select every scalar catalog value for the 150/180-cycle endpoint.
 *
 * Frequency is not advertised as a standardized aggregate capability, but
 * this endpoint has always carried the R5C1 mean as an explicitly informative
 * diagnostic. An explicit all-catalog selection preserves that field while
 * also retrieving the POWER/PHASOR/UNBAL sibling values for the same period.
 */
[[nodiscard]] inline mnc::meter::MeterSnapshotRequest
meter_aggregate_snapshot_selection()
{
	mnc::meter::MeterSnapshotRequest result{};
	result.period = mnc::meter::MeasurementPeriod::Cycles150_180;
	const auto attributes = mnc::meter::defined_attributes();
	result.attributes.assign(attributes.begin(), attributes.end());
	return result;
}

/** Project the typed 150/180-cycle provider view without recomputing values. */
[[nodiscard]] inline std::optional<MeterAggregateDto>
meter_aggregate_dto(const msap1::MeterSnapshotResponse &response)
{
	using Id = mnc::meter::MeterAttributeId;
	using Quality = mnc::meter::ReadingQuality;
	using Unit = mnc::meter::MeterUnit;
	if (!response.running || !response.has_snapshot)
		return std::nullopt;
	const auto &snapshot = response.snapshot;
	if (snapshot.period != mnc::meter::MeasurementPeriod::Cycles150_180)
		throw std::invalid_argument(
			"cached meter snapshot is not a 150/180-cycle aggregate");
	if (!snapshot.timing || !snapshot.timing->first_sample_index ||
	    !snapshot.timing->sample_count || !snapshot.timing->sample_rate_hz ||
	    !snapshot.timing->cycle_count ||
	    !snapshot.timing->nominal_frequency_hz ||
	    !snapshot.timing->source_interval_count ||
	    !snapshot.timing->first_source_sequence ||
	    !snapshot.timing->last_source_sequence)
		throw std::invalid_argument(
			"150/180-cycle aggregate has incomplete timing provenance");
	const auto &timing = *snapshot.timing;
	if (*timing.source_interval_count != meter::basic_blocks_per_aggregate ||
	    *timing.first_source_sequence >
		std::numeric_limits<std::uint32_t>::max() ||
	    *timing.last_source_sequence >
		std::numeric_limits<std::uint32_t>::max())
		throw std::invalid_argument(
			"150/180-cycle aggregate has invalid source provenance");

	const auto find = [&snapshot](Id id) -> const mnc::meter::MeterAttributeValue * {
		const auto it = std::find_if(snapshot.values.begin(), snapshot.values.end(),
			[id](const auto &value) {
				return value.attribute.id == id && !value.attribute.index;
			});
		return it == snapshot.values.end() ? nullptr : &*it;
	};
	const auto *frequency = find(Id::Frequency);
	if (!frequency || frequency->unit != Unit::MilliHertz ||
	    frequency->value < 0 ||
	    static_cast<std::uint64_t>(frequency->value) >
		std::numeric_limits<std::uint32_t>::max())
		throw std::invalid_argument(
			"150/180-cycle aggregate has no informative frequency");

	const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
	const auto age_ns = std::max<std::int64_t>(
		0, now - snapshot.updated_at_nanoseconds);
	const auto age_ms64 = age_ns / 1'000'000;
	MeterAggregateDto result{
		true,
		snapshot.sequence,
		snapshot.configuration_generation,
		*timing.sample_rate_hz,
		*timing.sample_count,
		*timing.first_sample_index,
		static_cast<std::uint32_t>(*timing.first_source_sequence),
		static_cast<std::uint32_t>(*timing.last_source_sequence),
		*timing.source_interval_count,
		*timing.cycle_count,
		*timing.nominal_frequency_hz,
		false,
		time_quality_name(timing.quality),
		static_cast<std::uint32_t>(std::min<std::int64_t>(
			age_ms64, std::numeric_limits<std::uint32_t>::max())),
		{},
		{},
		{static_cast<std::uint32_t>(frequency->value), true},
	};
	const std::array<Id, msap1::meter_channel_count> channel_ids{
		Id::IaRms, Id::IbRms, Id::IcRms, Id::InRms,
		Id::VcnRms, Id::VbnRms, Id::VanRms, Id::Frequency,
	};
	for (std::size_t index = 0; index < result.channels.size(); ++index) {
		const auto *reading = index == 7 ? nullptr : find(channel_ids[index]);
		const bool valid = reading && reading->quality == Quality::Valid;
		result.channels[index] = {
			static_cast<std::uint32_t>(index), meter_channel_names[index],
			meter_channel_unit(index), valid,
			valid ? meter_units(reading->value) : 0.0,
		};
	}
	for (const auto &reading : snapshot.values) {
		if (reading.quality == Quality::ArithmeticError)
			result.arithmetic_error = true;
		switch (reading.attribute.id) {
		case Id::Frequency:
		case Id::VanRms:
		case Id::VbnRms:
		case Id::VcnRms:
		case Id::IaRms:
		case Id::IbRms:
		case Id::IcRms:
		case Id::InRms:
			break;
		default:
			result.attributes.push_back(attribute_dto(reading));
			break;
		}
	}
	return result;
}

} // namespace msap1::web::api
