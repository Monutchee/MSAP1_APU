#pragma once

#include "msap1/meter/meter_data.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <string>

namespace msap1::web::api {

struct PhaseTotalStringDto {
	std::string phase_a;
	std::string phase_b;
	std::string phase_c;
	std::string total;
};

struct EnergyResponseDto {
	PhaseTotalStringDto active_import_uwh;
	PhaseTotalStringDto active_export_uwh;
	PhaseTotalStringDto apparent_uvah;
	PhaseTotalStringDto reactive_quadrant_i_uvarh;
	PhaseTotalStringDto reactive_quadrant_ii_uvarh;
	PhaseTotalStringDto reactive_quadrant_iii_uvarh;
	PhaseTotalStringDto reactive_quadrant_iv_uvarh;
	std::string session_id;
	std::string last_sample_index;
	std::string accepted_samples;
	std::string skipped_samples;
	std::uint32_t accepted_blocks = 0;
	std::uint32_t skipped_blocks = 0;
	std::string reset_epoch;
	std::string last_durable_update_nanoseconds;
	std::string quality;
	bool incomplete_accumulation = false;
	bool saturated = false;
	bool discontinuity = false;
};

struct DemandResponseDto {
	PhaseTotalStringDto current_active_uw;
	PhaseTotalStringDto import_peak_uw;
	PhaseTotalStringDto export_peak_uw;
	PhaseTotalStringDto import_peak_sample;
	PhaseTotalStringDto export_peak_sample;
	std::string session_id;
	std::string last_sample_index;
	std::string interval_anchor_sample;
	std::uint32_t source_interval_count = 0;
	std::uint32_t source_status = 0;
	std::string method;
	std::uint32_t window_seconds = 0;
	std::uint32_t update_seconds = 0;
	std::uint32_t profile_generation = 0;
	std::string peak_reset_epoch;
	std::string last_durable_update_nanoseconds;
	std::string quality;
	bool time_aligned = false;
	bool contaminated = false;
	bool boundary_valid = false;
	bool incomplete_accumulation = false;
	bool saturated = false;
};

template<typename Unit>
inline PhaseTotalStringDto exact_group(
	const PhaseABCTotal<Reading<Unit>> &values)
{
	return {std::to_string(values.phase_a.value),
		std::to_string(values.phase_b.value),
		std::to_string(values.phase_c.value),
		std::to_string(values.total.value)};
}

inline PhaseTotalStringDto exact_samples(
	const PhaseABCTotal<std::uint64_t> &values)
{
	return {std::to_string(values.phase_a), std::to_string(values.phase_b),
		std::to_string(values.phase_c), std::to_string(values.total)};
}

inline std::string quality_name(MeasurementQuality quality)
{
	switch (quality) {
	case MeasurementQuality::unavailable: return "unavailable";
	case MeasurementQuality::valid: return "valid";
	case MeasurementQuality::invalid: return "invalid";
	case MeasurementQuality::out_of_range: return "out_of_range";
	case MeasurementQuality::timed_out: return "timed_out";
	case MeasurementQuality::arithmetic_error: return "arithmetic_error";
	}
	return "unavailable";
}

inline EnergyResponseDto energy_dto(const EnergyValues &values)
{
	MeasurementQuality quality = MeasurementQuality::valid;
	const auto consider = [&quality](const auto &group) {
		for (const auto *reading : {&group.phase_a, &group.phase_b,
			&group.phase_c, &group.total})
			if (reading->quality != MeasurementQuality::valid)
				quality = reading->quality;
	};
	consider(values.active_import);
	consider(values.active_export);
	consider(values.apparent);
	for (const auto &quadrant : values.reactive_quadrants)
		consider(quadrant);
	const auto updated = std::chrono::duration_cast<std::chrono::nanoseconds>(
		values.active_import.phase_a.measured_at.time_since_epoch()).count();
	return {
		exact_group(values.active_import), exact_group(values.active_export),
		exact_group(values.apparent), exact_group(values.reactive_quadrants[0]),
		exact_group(values.reactive_quadrants[1]),
		exact_group(values.reactive_quadrants[2]),
		exact_group(values.reactive_quadrants[3]),
		std::to_string(values.session_id),
		std::to_string(values.last_sample_index),
		std::to_string(values.accepted_samples),
		std::to_string(values.skipped_samples),
		values.accepted_blocks, values.skipped_blocks,
		std::to_string(values.reset_epoch), std::to_string(updated),
		quality_name(quality), values.incomplete_input, values.saturated,
		values.discontinuity,
	};
}

inline DemandResponseDto demand_dto(const DemandValues &values)
{
	MeasurementQuality quality = MeasurementQuality::valid;
	for (const auto *reading : {&values.current_active.phase_a,
		&values.current_active.phase_b, &values.current_active.phase_c,
		&values.current_active.total})
		if (reading->quality != MeasurementQuality::valid)
			quality = reading->quality;
	const auto updated = std::chrono::duration_cast<std::chrono::nanoseconds>(
		values.current_active.phase_a.measured_at.time_since_epoch()).count();
	return {
		.current_active_uw = exact_group(values.current_active),
		.import_peak_uw = exact_group(values.import_peak),
		.export_peak_uw = exact_group(values.export_peak),
		.import_peak_sample = exact_samples(values.import_peak_sample),
		.export_peak_sample = exact_samples(values.export_peak_sample),
		.session_id = std::to_string(values.session_id),
		.last_sample_index = std::to_string(values.last_sample_index),
		.interval_anchor_sample =
			std::to_string(values.interval_anchor_sample),
		.source_interval_count = values.source_interval_count,
		.source_status = values.source_status,
		.method = values.method == DemandMethod::fixed_block
			? "fixed_block" : "sliding",
		.window_seconds = values.window_seconds,
		.update_seconds = values.update_seconds,
		.profile_generation = values.profile_generation,
		.peak_reset_epoch = std::to_string(values.peak_reset_epoch),
		.last_durable_update_nanoseconds = std::to_string(updated),
		.quality = quality_name(quality),
		.time_aligned = values.time_aligned,
		.contaminated = values.contaminated,
		.boundary_valid = values.boundary_valid,
		.incomplete_accumulation = values.incomplete_input,
		.saturated = values.saturated,
	};
}

} // namespace msap1::web::api
