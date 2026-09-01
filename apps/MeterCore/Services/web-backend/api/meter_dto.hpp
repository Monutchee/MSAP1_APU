#pragma once

/**
 * @file meter_dto.hpp
 * @brief Meter channel identity shared by every metering endpoint, plus the
 *        finalized aggregate transfer objects and their projections.
 *
 * The channel naming, units, and micro-unit scaling live here because
 * GET /api/v1/meter/readings, GET /api/v1/meter/aggregate,
 * GET /api/v1/meter/frequency-10s, and GET /api/v1/meter/minutes-10 must
 * present measurements and provenance consistently; duplicating the common
 * projections would let the documents drift.
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
#include <utility>
#include <vector>

namespace msap1::web::api {

/**
 * Hardware channel order of every MSAP1 basic and aggregate meter record:
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
	std::string quality;
	std::uint64_t source_sequence;
};

/** Stable JSON spelling for the provider's complete electrical quality. */
[[nodiscard]] inline const char *
reading_quality_name(mnc::meter::ReadingQuality quality)
{
	switch (quality) {
	case mnc::meter::ReadingQuality::Unavailable: return "unavailable";
	case mnc::meter::ReadingQuality::Valid: return "valid";
	case mnc::meter::ReadingQuality::Invalid: return "invalid";
	case mnc::meter::ReadingQuality::OutOfRange: return "out_of_range";
	case mnc::meter::ReadingQuality::TimedOut: return "timed_out";
	case mnc::meter::ReadingQuality::ArithmeticError:
		return "arithmetic_error";
	}
	return "unavailable";
}

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
	case mnc::meter::MeterUnit::MicroWattHours:
		value = static_cast<double>(reading.value);
		unit = "uWh";
		break;
	case mnc::meter::MeterUnit::MicroVarHours:
		value = static_cast<double>(reading.value);
		unit = "uvarh";
		break;
	case mnc::meter::MeterUnit::MicroVoltAmpereHours:
		value = static_cast<double>(reading.value);
		unit = "uVAh";
		break;
	case mnc::meter::MeterUnit::MicroWatts:
		value = static_cast<double>(reading.value);
		unit = "uW";
		break;
	case mnc::meter::MeterUnit::CrestTenThousandths:
		value = static_cast<double>(reading.value) / 10000.0;
		unit = "crest";
		break;
	case mnc::meter::MeterUnit::CategoricalCode:
		value = static_cast<double>(reading.value);
		unit = "code";
		break;
	}
	return {std::string(descriptor.key), unit,
		reading.quality == mnc::meter::ReadingQuality::Valid, value,
		reading_quality_name(reading.quality), reading.source_sequence};
}

/**
 * A typed snapshot is assembled from a fundamental record and its derived
 * siblings.  During the very short interval between those records, the store
 * can contain a new fundamental together with the previous derived values.
 * Consumers that require one atomic family must wait until every returned
 * derived value carries the snapshot sequence.
 */
[[nodiscard]] inline bool
derived_record_complete(const mnc::meter::MeterSnapshot &snapshot)
{
	using Id = mnc::meter::MeterAttributeId;
	bool has_derived_value = false;
	for (const auto &reading : snapshot.values) {
		switch (reading.attribute.id) {
		case Id::Frequency: case Id::VanRms: case Id::VbnRms:
		case Id::VcnRms: case Id::IaRms: case Id::IbRms:
		case Id::IcRms: case Id::InRms:
			continue;
		default:
			has_derived_value = true;
			if (reading.source_sequence != snapshot.sequence)
				return false;
			break;
		}
	}
	return has_derived_value;
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

struct Frequency10sFlagName {
	std::uint32_t bit;
	std::string_view name;
};

template<std::size_t Count>
[[nodiscard]] inline std::vector<std::string> frequency_10s_flag_names(
	std::uint32_t value,
	const std::array<Frequency10sFlagName, Count> &catalog)
{
	std::vector<std::string> result;
	for (const auto &[bit, name] : catalog)
		if ((value & bit) != 0u)
			result.emplace_back(name);
	return result;
}

inline constexpr std::array frequency_10s_status_catalog{
	Frequency10sFlagName{meter_frequency_10s_status_arithmetic_error,
		"arithmetic_error"},
	Frequency10sFlagName{meter_frequency_10s_status_result_valid,
		"result_valid"},
	Frequency10sFlagName{meter_frequency_10s_status_time_aligned,
		"time_aligned"},
	Frequency10sFlagName{meter_frequency_10s_status_profile_supported,
		"profile_supported"},
	Frequency10sFlagName{meter_frequency_10s_status_time_synchronized,
		"time_synchronized"},
	Frequency10sFlagName{meter_frequency_10s_status_filter_ready,
		"filter_ready"},
	Frequency10sFlagName{meter_frequency_10s_status_reference_valid,
		"reference_valid"},
	Frequency10sFlagName{meter_frequency_10s_status_discontinuity,
		"discontinuity"},
	Frequency10sFlagName{meter_frequency_10s_status_crossing_overflow,
		"crossing_overflow"},
	Frequency10sFlagName{meter_frequency_10s_status_observer_drop,
		"observer_drop"},
	Frequency10sFlagName{meter_frequency_10s_status_insufficient_crossings,
		"insufficient_crossings"},
	Frequency10sFlagName{meter_frequency_10s_status_out_of_range,
		"out_of_range"},
	Frequency10sFlagName{meter_frequency_10s_status_transport_gap,
		"transport_gap"},
	Frequency10sFlagName{meter_frequency_10s_status_calibration_valid,
		"calibration_valid"},
	Frequency10sFlagName{meter_frequency_10s_status_sample_rate_valid,
		"sample_rate_valid"},
	Frequency10sFlagName{meter_frequency_10s_status_resynchronized,
		"resynchronized"},
};

inline constexpr std::array frequency_10s_reason_catalog{
	Frequency10sFlagName{meter_frequency_10s_reason_unsupported_profile,
		"unsupported_profile"},
	Frequency10sFlagName{meter_frequency_10s_reason_time_unsynchronized,
		"time_unsynchronized"},
	Frequency10sFlagName{meter_frequency_10s_reason_time_uncertainty,
		"time_uncertainty"},
	Frequency10sFlagName{meter_frequency_10s_reason_filter_warmup,
		"filter_warmup"},
	Frequency10sFlagName{meter_frequency_10s_reason_reference_invalid,
		"reference_invalid"},
	Frequency10sFlagName{meter_frequency_10s_reason_discontinuity,
		"discontinuity"},
	Frequency10sFlagName{meter_frequency_10s_reason_crossing_overflow,
		"crossing_overflow"},
	Frequency10sFlagName{meter_frequency_10s_reason_observer_drop,
		"observer_drop"},
	Frequency10sFlagName{meter_frequency_10s_reason_sample_rate_invalid,
		"sample_rate_invalid"},
	Frequency10sFlagName{meter_frequency_10s_reason_boundary_invalid,
		"boundary_invalid"},
	Frequency10sFlagName{meter_frequency_10s_reason_calibration_invalid,
		"calibration_invalid"},
	Frequency10sFlagName{meter_frequency_10s_reason_insufficient_crossings,
		"insufficient_crossings"},
	Frequency10sFlagName{meter_frequency_10s_reason_out_of_range,
		"out_of_range"},
	Frequency10sFlagName{meter_frequency_10s_reason_arithmetic,
		"arithmetic"},
	Frequency10sFlagName{meter_frequency_10s_reason_transport_gap,
		"transport_gap"},
	Frequency10sFlagName{meter_frequency_10s_reason_cycle_geometry,
		"cycle_geometry"},
	Frequency10sFlagName{meter_frequency_10s_reason_time_geometry,
		"time_geometry"},
};

inline constexpr std::array frequency_10s_source_status_catalog{
	Frequency10sFlagName{1u << 0u, "boundary_valid"},
	Frequency10sFlagName{1u << 1u, "time_synchronized"},
	Frequency10sFlagName{1u << 2u, "sample_rate_valid"},
	Frequency10sFlagName{1u << 3u, "filter_ready"},
	Frequency10sFlagName{1u << 4u, "reference_valid"},
	Frequency10sFlagName{1u << 5u, "source_discontinuity"},
	Frequency10sFlagName{1u << 6u, "crossing_overflow"},
	Frequency10sFlagName{1u << 7u, "observer_drop"},
	Frequency10sFlagName{1u << 8u, "resynchronized"},
	Frequency10sFlagName{1u << 9u, "calibration_valid"},
	Frequency10sFlagName{1u << 10u, "profile_supported"},
};

inline constexpr std::array frequency_10s_guard_catalog{
	Frequency10sFlagName{1u << 0u, "before_start"},
	Frequency10sFlagName{1u << 1u, "after_end"},
	Frequency10sFlagName{1u << 2u, "exact_start"},
	Frequency10sFlagName{1u << 3u, "exact_end"},
};

/** Available body of GET /api/v1/meter/frequency-10s. */
struct MeterFrequency10sDto {
	bool available;
	std::uint32_t sequence;
	std::uint32_t configuration_generation;
	bool valid;
	std::string quality;
	std::optional<double> frequency_hz;
	std::optional<std::uint32_t> frequency_millihz;
	std::string time_quality;
	std::uint32_t age_ms;
	std::string first_sample_index;
	std::string interval_end_sample_index;
	std::uint32_t sample_count;
	std::uint32_t sample_rate_hz;
	std::uint32_t measured_sample_rate_millihz;
	std::uint32_t cycle_count;
	std::string utc_start_nanoseconds;
	std::string utc_end_nanoseconds;
	std::string utc_uncertainty_nanoseconds;
	std::uint32_t source_sequence;
	std::uint32_t boundary_generation;
	std::uint32_t source_status;
	std::vector<std::string> source_status_flags;
	std::uint32_t status;
	std::vector<std::string> status_flags;
	std::uint32_t reasons;
	std::vector<std::string> rejection_reasons;
	std::uint32_t observer_drop_count;
	std::uint32_t guard_flags;
	std::vector<std::string> guard_flag_names;
	std::uint32_t observed_crossings;
	std::uint32_t included_crossings;
	std::uint32_t rejected_cycles;
	std::string duration_q16_samples;
	std::string first_crossing_q16_samples;
	std::string last_crossing_q16_samples;
	std::uint32_t nominal_frequency_hz;
	std::uint32_t reference_channel;
	std::uint32_t filter_profile;
	std::uint32_t calibration_profile;
};

/** No complete UTC ten-second result exists yet. */
struct MeterFrequency10sUnavailableDto {
	bool available = false;
};

/** Select only the standardized typed frequency value and its audit block. */
[[nodiscard]] inline mnc::meter::MeterSnapshotRequest
meter_frequency_10s_snapshot_selection()
{
	return {
		.period = mnc::meter::MeasurementPeriod::Seconds10,
		.attributes = {{mnc::meter::MeterAttributeId::Frequency,
			std::nullopt}},
	};
}

/** Project an already-validated R5C1 ten-second result without recomputing it. */
[[nodiscard]] inline std::optional<MeterFrequency10sDto>
meter_frequency_10s_dto(const msap1::MeterSnapshotResponse &response)
{
	using Id = mnc::meter::MeterAttributeId;
	using Quality = mnc::meter::ReadingQuality;
	using Unit = mnc::meter::MeterUnit;
	if (!response.running || !response.has_snapshot)
		return std::nullopt;
	const auto &snapshot = response.snapshot;
	if (snapshot.period != mnc::meter::MeasurementPeriod::Seconds10)
		throw std::invalid_argument(
			"cached meter snapshot is not a ten-second frequency result");
	if (!snapshot.timing || !snapshot.frequency_10s ||
	    !snapshot.timing->utc_start_nanoseconds ||
	    !snapshot.timing->utc_uncertainty_nanoseconds ||
	    !snapshot.timing->first_sample_index ||
	    !snapshot.timing->sample_count ||
	    !snapshot.timing->sample_rate_hz ||
	    !snapshot.timing->cycle_count ||
	    !snapshot.timing->nominal_frequency_hz)
		throw std::invalid_argument(
			"ten-second frequency has incomplete timing or audit provenance");
	const auto &timing = *snapshot.timing;
	const auto &audit = *snapshot.frequency_10s;
	const auto frequency = std::find_if(snapshot.values.begin(),
		snapshot.values.end(), [](const auto &reading) {
			return reading.attribute.id == Id::Frequency &&
				!reading.attribute.index;
		});
	if (frequency == snapshot.values.end() ||
	    std::count_if(snapshot.values.begin(), snapshot.values.end(),
		[](const auto &reading) {
			return reading.attribute.id == Id::Frequency &&
				!reading.attribute.index;
		}) != 1 ||
	    frequency->unit != Unit::MilliHertz || frequency->value < 0 ||
	    static_cast<std::uint64_t>(frequency->value) >
		std::numeric_limits<std::uint32_t>::max())
		throw std::invalid_argument(
			"ten-second frequency has no unique millihertz value");
	if (snapshot.sequence > std::numeric_limits<std::uint32_t>::max() ||
	    frequency->source_sequence != snapshot.sequence ||
	    audit.source_sequence != snapshot.sequence)
		throw std::invalid_argument(
			"ten-second frequency source sequence disagrees");
	if (*timing.sample_count == 0u || *timing.sample_rate_hz == 0u ||
	    *timing.first_sample_index >
		std::numeric_limits<std::uint64_t>::max() -
		*timing.sample_count ||
	    *timing.first_sample_index + *timing.sample_count !=
		audit.interval_end_sample_index ||
	    frequency->sample_count != *timing.sample_count ||
	    frequency->calculation_window_nanoseconds != 10'000'000'000ll)
		throw std::invalid_argument(
			"ten-second frequency sample interval disagrees");
	if (*timing.utc_start_nanoseconds < 0 ||
	    static_cast<std::uint64_t>(*timing.utc_start_nanoseconds) !=
		audit.utc_start_nanoseconds ||
	    *timing.utc_uncertainty_nanoseconds !=
		audit.utc_uncertainty_nanoseconds ||
	    audit.utc_start_nanoseconds >
		std::numeric_limits<std::uint64_t>::max() - 10'000'000'000ull ||
	    audit.utc_start_nanoseconds + 10'000'000'000ull !=
		audit.utc_end_nanoseconds ||
	    audit.utc_end_nanoseconds >
		static_cast<std::uint64_t>(
			std::numeric_limits<std::int64_t>::max()) ||
	    frequency->measured_at_nanoseconds !=
		static_cast<std::int64_t>(audit.utc_end_nanoseconds))
		throw std::invalid_argument(
			"ten-second frequency UTC interval disagrees");
	if ((audit.source_status & ~meter_frequency_10s_source_status_mask) != 0u ||
	    (audit.status & ~meter_frequency_10s_status_mask) != 0u ||
	    (audit.reasons & ~meter_frequency_10s_reason_mask) != 0u ||
	    (static_cast<std::uint32_t>(audit.guard_flags) &
		~meter_frequency_10s_guard_flags_mask) != 0u ||
	    audit.nominal_frequency_hz != *timing.nominal_frequency_hz)
		throw std::invalid_argument(
			"ten-second frequency audit flags or profile disagree");
	const bool valid = frequency->quality == Quality::Valid;
	const bool invalid_quality = frequency->quality == Quality::Invalid ||
		frequency->quality == Quality::OutOfRange ||
		frequency->quality == Quality::ArithmeticError;
	const bool result_valid =
		(audit.status & meter_frequency_10s_status_result_valid) != 0u;
	if ((!valid && !invalid_quality) || valid != result_valid ||
	    valid != (audit.reasons == 0u) ||
	    (valid && frequency->value == 0) ||
	    (!valid && frequency->value != 0))
		throw std::invalid_argument(
			"ten-second frequency quality disagrees with its audit status");

	const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
	const auto age_ns = std::max<std::int64_t>(
		0, now - snapshot.updated_at_nanoseconds);
	const auto age_ms = static_cast<std::uint32_t>(
		std::min<std::int64_t>(age_ns / 1'000'000,
			std::numeric_limits<std::uint32_t>::max()));
	const auto millihz = static_cast<std::uint32_t>(frequency->value);
	return MeterFrequency10sDto{
		.available = true,
		.sequence = static_cast<std::uint32_t>(snapshot.sequence),
		.configuration_generation = snapshot.configuration_generation,
		.valid = valid,
		.quality = reading_quality_name(frequency->quality),
		.frequency_hz = valid
			? std::optional<double>{static_cast<double>(millihz) / 1000.0}
			: std::nullopt,
		.frequency_millihz = valid
			? std::optional<std::uint32_t>{millihz} : std::nullopt,
		.time_quality = time_quality_name(timing.quality),
		.age_ms = age_ms,
		.first_sample_index = std::to_string(*timing.first_sample_index),
		.interval_end_sample_index =
			std::to_string(audit.interval_end_sample_index),
		.sample_count = *timing.sample_count,
		.sample_rate_hz = *timing.sample_rate_hz,
		.measured_sample_rate_millihz =
			audit.measured_sample_rate_millihz,
		.cycle_count = *timing.cycle_count,
		.utc_start_nanoseconds =
			std::to_string(audit.utc_start_nanoseconds),
		.utc_end_nanoseconds = std::to_string(audit.utc_end_nanoseconds),
		.utc_uncertainty_nanoseconds =
			std::to_string(audit.utc_uncertainty_nanoseconds),
		.source_sequence = audit.source_sequence,
		.boundary_generation = audit.boundary_generation,
		.source_status = audit.source_status,
		.source_status_flags = frequency_10s_flag_names(
			audit.source_status, frequency_10s_source_status_catalog),
		.status = audit.status,
		.status_flags = frequency_10s_flag_names(
			audit.status, frequency_10s_status_catalog),
		.reasons = audit.reasons,
		.rejection_reasons = frequency_10s_flag_names(
			audit.reasons, frequency_10s_reason_catalog),
		.observer_drop_count = audit.observer_drop_count,
		.guard_flags = audit.guard_flags,
		.guard_flag_names = frequency_10s_flag_names(
			audit.guard_flags, frequency_10s_guard_catalog),
		.observed_crossings = audit.observed_crossings,
		.included_crossings = audit.included_crossings,
		.rejected_cycles = audit.rejected_cycles,
		.duration_q16_samples = std::to_string(audit.duration_q16_samples),
		.first_crossing_q16_samples =
			std::to_string(audit.first_crossing_q16_samples),
		.last_crossing_q16_samples =
			std::to_string(audit.last_crossing_q16_samples),
		.nominal_frequency_hz = audit.nominal_frequency_hz,
		.reference_channel = audit.reference_channel,
		.filter_profile = audit.filter_profile,
		.calibration_profile = audit.calibration_profile,
	};
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
 * own 10 s interval, exposed separately by `/api/v1/meter/frequency-10s`.
 * This object deliberately carries no `valid` flag so no consumer can mistake
 * the mean of the 15 basic estimates for the standardized measurement.
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
	bool record_complete = false;
	std::optional<std::int64_t> utc_start_nanoseconds;
	std::optional<std::uint64_t> utc_uncertainty_nanoseconds;
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
	bool record_complete = false;
	std::optional<std::int64_t> utc_start_nanoseconds;
	std::optional<std::uint64_t> utc_uncertainty_nanoseconds;
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
		false, false, false, false, std::nullopt, std::nullopt,
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
	result.record_complete = derived_record_complete(snapshot);
	result.utc_start_nanoseconds = snapshot.timing->utc_start_nanoseconds;
	result.utc_uncertainty_nanoseconds =
		snapshot.timing->utc_uncertainty_nanoseconds;
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
 * Select the supported scalar values for the 150/180-cycle endpoint.
 *
 * Frequency is not advertised as a standardized aggregate capability, but
 * this endpoint has always carried the R5C1 mean as an explicitly informative
 * diagnostic. Request it explicitly beside the period-capable values. Asking
 * the provider for unrelated energy or demand values would return explicit
 * unavailable readings whose absent provenance must not make an otherwise
 * coherent aggregate family appear incomplete.
 */
[[nodiscard]] inline mnc::meter::MeterSnapshotRequest
meter_aggregate_snapshot_selection()
{
	mnc::meter::MeterSnapshotRequest result{};
	result.period = mnc::meter::MeasurementPeriod::Cycles150_180;
	const auto attributes = mnc::meter::attributes_for(result.period,
		mnc::meter::MeterAttributeUsage::Snapshot);
	result.attributes.reserve(attributes.size() + 1u);
	result.attributes.push_back({mnc::meter::MeterAttributeId::Frequency,
		std::nullopt});
	result.attributes.insert(result.attributes.end(), attributes.begin(),
		attributes.end());
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
		false,
		std::nullopt,
		std::nullopt,
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
	result.record_complete = derived_record_complete(snapshot);
	result.utc_start_nanoseconds = timing.utc_start_nanoseconds;
	result.utc_uncertainty_nanoseconds =
		timing.utc_uncertainty_nanoseconds;
	return result;
}

} // namespace msap1::web::api
