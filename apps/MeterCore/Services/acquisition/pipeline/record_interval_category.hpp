#pragma once

/**
 * @file record_interval_category.hpp
 * @brief Human and structured interval identity for meter-record diagnostics.
 */

#include "msap1/meter/meter_record.hpp"
#include "mnc/MeterDataProvider/attributes/meter_attribute.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace msap1::acquisition::daemon {

enum class RecordIntervalCategory : std::uint8_t {
	unknown = 0,
	single_cycle,
	urms_half,
	basic,
	seconds_10,
	cycles_150_180,
	minutes_10,
	hours_2,
	minutes_10_live,
	hours_2_live,
	demand,
	flicker,
	mains_signal,
	count,
};

inline constexpr std::size_t record_interval_category_count =
	static_cast<std::size_t>(RecordIntervalCategory::count);

struct RecordIntervalIdentity {
	RecordIntervalCategory category = RecordIntervalCategory::unknown;
	std::string_view code = "unknown";
	std::string_view label = "unknown interval";
};

/**
 * Resolve the interval from the format before full decoding.
 *
 * Rejection diagnostics need this even when the record body is malformed.
 * HARMONIC-AGG is the one multiplexed format, so its period selector is read
 * directly from the shape word; an invalid selector remains explicitly
 * unknown rather than being guessed.
 */
[[nodiscard]] inline RecordIntervalIdentity record_interval_identity(
	const msap1::MeterRecord &record)
{
	switch (record.record_format()) {
	case msap1::meter_single_cycle_format:
		return {RecordIntervalCategory::single_cycle, "single_cycle",
			"single-cycle"};
	case msap1::meter_pq_event_format:
		return {RecordIntervalCategory::urms_half, "urms_half",
			"Urms(1/2)"};
	case msap1::meter_flicker_format:
		return {RecordIntervalCategory::flicker, "flicker",
			"flicker interval"};
	case msap1::meter_mains_signal_format:
		return {RecordIntervalCategory::mains_signal, "mains_signal",
			"mains-signalling observation"};
	case msap1::meter_periodic_format:
	case msap1::meter_power_format:
	case msap1::meter_phasor_format:
	case msap1::meter_unbalance_format:
	case msap1::meter_energy_format:
	case msap1::meter_harmonic_format:
		return {RecordIntervalCategory::basic, "basic",
			"10/12-cycle"};
	case msap1::meter_frequency_10s_format:
		return {RecordIntervalCategory::seconds_10, "seconds_10",
			"UTC 10-second frequency"};
	case msap1::meter_aggregate_format:
	case msap1::meter_aggregate_power_format:
	case msap1::meter_aggregate_phasor_format:
	case msap1::meter_aggregate_unbalance_format:
		return {RecordIntervalCategory::cycles_150_180,
			"cycles_150_180", "150/180-cycle"};
	case msap1::meter_ten_minute_format:
	case msap1::meter_ten_minute_power_format:
	case msap1::meter_ten_minute_phasor_format:
	case msap1::meter_ten_minute_unbalance_format:
		return {RecordIntervalCategory::minutes_10, "minutes_10",
			"10-minute"};
	case msap1::meter_demand_format:
		return {RecordIntervalCategory::demand, "demand",
			"configured demand"};
	case msap1::meter_two_hour_format:
	case msap1::meter_two_hour_power_format:
	case msap1::meter_two_hour_phasor_format:
	case msap1::meter_two_hour_unbalance_format:
		return {RecordIntervalCategory::hours_2, "hours_2", "2-hour"};
	case msap1::meter_ten_minute_open_format:
	case msap1::meter_ten_minute_open_power_format:
	case msap1::meter_ten_minute_open_phasor_format:
	case msap1::meter_ten_minute_open_unbalance_format:
		return {RecordIntervalCategory::minutes_10_live,
			"minutes_10_live", "10-minute live partial"};
	case msap1::meter_two_hour_open_format:
	case msap1::meter_two_hour_open_power_format:
	case msap1::meter_two_hour_open_phasor_format:
	case msap1::meter_two_hour_open_unbalance_format:
		return {RecordIntervalCategory::hours_2_live, "hours_2_live",
			"2-hour live partial"};
	case msap1::meter_harmonic_aggregate_format:
		switch (record.word(14) & 0x3u) {
		case static_cast<std::uint32_t>(
			mnc::meter::MeasurementPeriod::Cycles150_180):
			return {RecordIntervalCategory::cycles_150_180,
				"cycles_150_180", "150/180-cycle"};
		case static_cast<std::uint32_t>(
			mnc::meter::MeasurementPeriod::Min10):
			return {RecordIntervalCategory::minutes_10, "minutes_10",
				"10-minute"};
		case static_cast<std::uint32_t>(
			mnc::meter::MeasurementPeriod::Hour2):
			return {RecordIntervalCategory::hours_2, "hours_2",
				"2-hour"};
		default:
			return {};
		}
	default:
		return {};
	}
}

} // namespace msap1::acquisition::daemon
