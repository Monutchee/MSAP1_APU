#pragma once

/**
 * @file harmonic_dto.hpp
 * @brief WebEngine-free projection of an atomic harmonic family.
 */

#include "msap1/acquisition/ipc/acquisition_commands.hpp"
#include "msap1/meter/harmonic_spectrum.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace msap1::web::api {

struct HarmonicOrderDto {
	std::uint32_t order = 0;
	std::uint64_t magnitude_micro_units = 0;
	double magnitude = 0.0;
	bool magnitude_valid = false;
	std::uint32_t angle_millidegrees = 0;
	double angle_degrees = 0.0;
	bool angle_valid = false;
};

struct HarmonicDistortionDto {
	std::variant<std::nullptr_t, double> percent{nullptr};
	std::uint32_t first_order = 2;
	std::uint32_t last_order = 50;
	std::string status = "interval_invalid";
};

struct HarmonicChannelDto {
	std::uint32_t channel = 0;
	std::string name;
	std::string unit;
	HarmonicDistortionDto thd{};
	std::vector<HarmonicOrderDto> orders;
};

struct HarmonicDto {
	bool running = false;
	bool available = false;
	std::uint64_t records = 0;
	std::uint64_t families = 0;
	std::uint64_t incomplete_families = 0;
	std::string period = "cycles_150_180";
	std::uint32_t sequence = 0;
	std::uint32_t configuration_generation = 0;
	std::uint32_t sample_rate_hz = 0;
	std::uint32_t sample_count = 0;
	std::uint64_t first_sample = 0;
	std::uint32_t measured_frequency_millihz = 0;
	std::uint32_t qualified_max_order = 0;
	std::uint32_t nominal_frequency_hz = 0;
	std::uint32_t cycle_count = 0;
	std::uint32_t filter_profile_id = 0;
	std::uint32_t valid_mask = 0;
	std::uint32_t status = 0;
	std::uint32_t emit_drops = 0;
	std::uint32_t result_drops = 0;
	std::uint64_t target_sample = 0;
	std::uint32_t contributors = 0;
	std::uint32_t overshoot_samples = 0;
	std::uint32_t first_source_sequence = 0;
	std::uint32_t last_source_sequence = 0;
	bool time_aligned = false;
	bool contaminated = false;
	bool interval_valid = false;
	bool arithmetic_error = false;
	bool grid_locked = false;
	bool conditioner_valid = false;
	bool fft_valid = false;
	bool full_range = false;
	bool first_after_discontinuity = false;
	bool rate_limited = false;
	std::vector<HarmonicChannelDto> channels;
};

[[nodiscard]] inline const char *harmonic_distortion_status_name(
	msap1::HarmonicDistortionStatus status)
{
	switch (status) {
	case msap1::HarmonicDistortionStatus::valid:
		return "valid";
	case msap1::HarmonicDistortionStatus::interval_invalid:
		return "interval_invalid";
	case msap1::HarmonicDistortionStatus::channel_unavailable:
		return "channel_unavailable";
	case msap1::HarmonicDistortionStatus::fundamental_unavailable:
		return "fundamental_unavailable";
	case msap1::HarmonicDistortionStatus::insufficient_order_range:
		return "insufficient_order_range";
	case msap1::HarmonicDistortionStatus::harmonic_unavailable:
		return "harmonic_unavailable";
	}
	return "interval_invalid";
}

[[nodiscard]] inline std::string harmonic_period_name(
	mnc::meter::MeasurementPeriod period)
{
	switch (period) {
	case mnc::meter::MeasurementPeriod::Basic:
		return "basic";
	case mnc::meter::MeasurementPeriod::Cycles150_180:
		return "cycles_150_180";
	case mnc::meter::MeasurementPeriod::Min10:
		return "minutes_10";
	case mnc::meter::MeasurementPeriod::Hour2:
		return "hours_2";
	default:
		throw std::invalid_argument("unsupported harmonic period");
	}
}

[[nodiscard]] inline HarmonicDto harmonic_dto(
	const msap1::HarmonicResponse &response)
{
	static constexpr std::array<const char *, 7> names{
		"Ia", "Ib", "Ic", "In", "Vc", "Vb", "Va"};
	HarmonicDto dto{};
	dto.running = response.running;
	dto.available = response.has_snapshot;
	dto.records = response.records;
	dto.families = response.families;
	dto.incomplete_families = response.incomplete_families;
	dto.period = harmonic_period_name(response.period);
	if (!response.has_snapshot)
		return dto;

	const auto &snapshot = response.snapshot;
	dto.sequence = snapshot.sequence;
	dto.configuration_generation = snapshot.configuration_generation;
	dto.sample_rate_hz = snapshot.sample_rate_hz;
	dto.sample_count = snapshot.sample_count;
	dto.first_sample = snapshot.first_sample;
	dto.measured_frequency_millihz =
		snapshot.measured_frequency_millihz;
	dto.qualified_max_order = snapshot.qualified_max_order;
	dto.nominal_frequency_hz = snapshot.nominal_frequency_hz;
	dto.cycle_count = snapshot.cycle_count;
	dto.filter_profile_id = snapshot.filter_profile_id;
	dto.valid_mask = snapshot.valid_mask;
	dto.status = snapshot.status;
	dto.emit_drops = snapshot.emit_drops;
	dto.result_drops = snapshot.result_drops;
	dto.target_sample = snapshot.target_sample;
	dto.contributors = snapshot.contributors;
	dto.overshoot_samples = snapshot.overshoot_samples;
	dto.first_source_sequence = snapshot.first_source_sequence;
	dto.last_source_sequence = snapshot.last_source_sequence;
	dto.time_aligned = snapshot.aligned;
	dto.contaminated = snapshot.contaminated;
	dto.interval_valid = snapshot.interval_valid();
	dto.arithmetic_error = snapshot.arithmetic_error();
	dto.grid_locked = snapshot.grid_locked();
	dto.conditioner_valid = snapshot.conditioner_valid();
	dto.fft_valid = snapshot.fft_valid();
	dto.full_range = snapshot.full_range();
	dto.first_after_discontinuity =
		snapshot.first_after_discontinuity();
	dto.rate_limited = snapshot.rate_limited();
	dto.channels.reserve(snapshot.channels.size());
	for (std::size_t channel = 0; channel < snapshot.channels.size();
	     ++channel) {
		HarmonicChannelDto channel_dto{};
		channel_dto.channel = static_cast<std::uint32_t>(channel);
		channel_dto.name = names[channel];
		channel_dto.unit = channel < 4 ? "A" : "V";
		const auto distortion = msap1::harmonic_distortion(snapshot, channel);
		channel_dto.thd = {
			distortion.percent
				? std::variant<std::nullptr_t, double>{*distortion.percent}
				: std::variant<std::nullptr_t, double>{nullptr},
			distortion.first_order,
			distortion.last_order,
			harmonic_distortion_status_name(distortion.status),
		};
		channel_dto.orders.reserve(harmonic_max_order);
		for (const auto &point : snapshot.channels[channel])
			channel_dto.orders.push_back({
				point.order,
				point.magnitude_micro_units,
				static_cast<double>(point.magnitude_micro_units) / 1e6,
				point.magnitude_valid,
				point.angle_millidegrees,
				static_cast<double>(point.angle_millidegrees) / 1000.0,
				point.angle_valid,
			});
		dto.channels.push_back(std::move(channel_dto));
	}
	return dto;
}

} // namespace msap1::web::api
