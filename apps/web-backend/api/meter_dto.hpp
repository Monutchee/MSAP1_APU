#pragma once

/**
 * @file meter_dto.hpp
 * @brief Meter channel identity shared by every metering endpoint, plus the
 *        150/180-cycle aggregate transfer objects and their projection.
 *
 * The channel naming, units, and micro-unit scaling live here because
 * GET /api/v1/meter/readings and GET /api/v1/meter/aggregate must present
 * the same channels the same way; duplicating the table would let the two
 * documents drift.
 *
 * The aggregate projection is deliberately free of WebEngine: it maps one
 * acquisition InfoResponse onto the response DTO and nothing else, so the
 * pinned JSON contract is directly testable without an HTTP stack.
 */

#include "msap1/acquisition/ipc/acquisition_ipc.hpp"
#include "msap1/meter/meter_data.hpp"
#include "msap1/meter/meter_record.hpp"
#include "msap1/meter/meter_timing.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

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

/**
 * @brief Project the cached MTR2 record of @p response onto the aggregate DTO.
 *
 * The record is decoded through the shared decoder registry rather than read
 * word by word, so the endpoint inherits the decoder's identity validation
 * and its aggregate RMS quality rules (an aggregation arithmetic error
 * outranks the per-channel valid mask).
 *
 * @return The rendered aggregate, or std::nullopt when none is available.
 * @throws std::invalid_argument when the cached record is malformed.
 */
[[nodiscard]] inline std::optional<MeterAggregateDto>
meter_aggregate_dto(const msap1::InfoResponse &response)
{
	if (!response.running || !response.has_aggregate_record)
		return std::nullopt;
	/* One immutable registry for the process: building the decoder table
	 * per request would allocate on every poll. */
	static const msap1::MeterDecoderRegistry decoders =
		msap1::MeterDecoderRegistry::with_builtin_decoders();
	const auto &record = response.latest_aggregate_record;
	const auto update = decoders.decode(record);
	if (!update.aggregate_timing || !update.fundamental ||
	    update.period != msap1::MeasurementPeriod::Cycles150_180)
		throw std::invalid_argument(
			"cached meter record is not a 150/180-cycle aggregate");
	const auto &timing = *update.aggregate_timing;
	const auto &fundamental = *update.fundamental;

	/* Decoded readings, back in hardware channel order. Channel 7 (VCM)
	 * is a debug channel the aggregate record does not measure, so it is
	 * reported present-but-invalid rather than as a valid zero. */
	struct ChannelSource {
		std::int64_t micro_units;
		bool valid;
	};
	const std::array<ChannelSource, msap1::meter_channel_count> sources{{
		{fundamental.current.phase_a.value,
		 fundamental.current.phase_a.valid()},
		{fundamental.current.phase_b.value,
		 fundamental.current.phase_b.valid()},
		{fundamental.current.phase_c.value,
		 fundamental.current.phase_c.valid()},
		{fundamental.current.neutral.value,
		 fundamental.current.neutral.valid()},
		{fundamental.voltage_ln.phase_c.value,
		 fundamental.voltage_ln.phase_c.valid()},
		{fundamental.voltage_ln.phase_b.value,
		 fundamental.voltage_ln.phase_b.valid()},
		{fundamental.voltage_ln.phase_a.value,
		 fundamental.voltage_ln.phase_a.valid()},
		{0, false},
	}};

	MeterAggregateDto result{
		true,
		timing.sequence,
		timing.configuration_generation,
		record.sample_rate_hz(),
		timing.sample_count,
		timing.first_sample_index,
		timing.first_basic_sequence,
		timing.last_basic_sequence,
		timing.basic_block_count,
		timing.cycle_count,
		static_cast<std::uint32_t>(timing.nominal_frequency),
		timing.arithmetic_error,
		/* Measurement-time provenance, NOT the daemon's live clock
		 * state: this is the quality stamped when THIS aggregate was
		 * ingested, carried across IPC beside the cached record.
		 * response.time_quality describes the moment of the HTTP
		 * request instead, so an aggregate measured while
		 * synchronized but read back during holdover would be
		 * mislabelled by it. Do not swap this back. The decoded
		 * timing above cannot supply it either — the raw PL record
		 * holds no UTC state, so the registry decoder leaves
		 * timing.time_quality at its default. */
		time_quality_name(response.aggregate_time_quality),
		response.aggregate_record_age_ms,
		{},
		{static_cast<std::uint32_t>(fundamental.frequency.value), true},
	};
	for (std::size_t index = 0; index < result.channels.size(); ++index)
		result.channels[index] = {
			static_cast<std::uint32_t>(index),
			meter_channel_names[index],
			meter_channel_unit(index),
			sources[index].valid,
			sources[index].valid
				? meter_units(sources[index].micro_units)
				: 0.0,
		};
	return result;
}

} // namespace msap1::web::api
