#include "msap1/meter/energy_demand.hpp"

#include <array>
#include <climits>
#include <limits>
#include <stdexcept>

namespace msap1 {
namespace {

constexpr std::uint32_t energy_header_allowed_mask = 0x00000f1fu;
constexpr std::uint32_t energy_status_allowed_mask = 0x0000001fu;
constexpr std::uint32_t demand_header_allowed_mask = 0x000fffffu;
constexpr std::uint32_t demand_status_allowed_mask = 0x0000007fu;

void require(bool condition, const char *message)
{
	if (!condition)
		throw std::invalid_argument(message);
}

void require_zero_words(const MeterRecord &record, std::size_t first,
	std::size_t last, const char *message)
{
	for (auto word = first; word <= last; ++word)
		require(record.word(word) == 0u, message);
}

std::int64_t checked_counter(const MeterRecord &record, std::size_t word)
{
	const auto value = record.unsigned64(word);
	require(value <= static_cast<std::uint64_t>(INT64_MAX),
		"energy/demand counter exceeds INT64_MAX");
	return static_cast<std::int64_t>(value);
}

SampleWindow source_window(const MeterRecord &record)
{
	require(record.sample_rate_hz() != 0u, "energy/demand sample rate is zero");
	const auto nanoseconds =
		(static_cast<std::uint64_t>(record.block_sample_count()) *
		 1000000000ULL) / record.sample_rate_hz();
	require(nanoseconds <= static_cast<std::uint64_t>(
		std::numeric_limits<std::int64_t>::max()),
		"energy/demand source window exceeds duration range");
	return {record.block_sample_count(),
		std::chrono::nanoseconds(static_cast<std::int64_t>(nanoseconds))};
}

template<typename Unit>
Reading<Unit> counter_reading(const MeterRecord &record, std::size_t word,
	bool valid, bool saturated, SystemTime received_at,
	const SampleWindow &window)
{
	Reading<Unit> result;
	result.value = checked_counter(record, word);
	result.quality = !valid ? MeasurementQuality::invalid
		: saturated ? MeasurementQuality::out_of_range
			    : MeasurementQuality::valid;
	result.source_sequence = record.sequence();
	result.measured_at = received_at;
	result.calculation_window = window;
	return result;
}

Reading<MicroWatts> signed_demand_reading(const MeterRecord &record,
	std::size_t word, bool valid, bool saturated, SystemTime received_at,
	const SampleWindow &window)
{
	Reading<MicroWatts> result;
	result.value = record.signed64(word);
	result.quality = !valid ? MeasurementQuality::invalid
		: saturated ? MeasurementQuality::out_of_range
			    : MeasurementQuality::valid;
	result.source_sequence = record.sequence();
	result.measured_at = received_at;
	result.calculation_window = window;
	return result;
}

template<typename T>
T &phase_total(T &phase_a, T &phase_b, T &phase_c, T &total,
	std::size_t index)
{
	switch (index) {
	case 0: return phase_a;
	case 1: return phase_b;
	case 2: return phase_c;
	case 3: return total;
	default: throw std::out_of_range("phase/total index");
	}
}

template<typename T>
T &phase_total(PhaseABCTotal<T> &values, std::size_t index)
{
	return phase_total(values.phase_a, values.phase_b, values.phase_c,
		values.total, index);
}

void validate_energy_part(const MeterRecord &record)
{
	require(record.header_valid() &&
		record.record_format() == meter_energy_format,
		"invalid ENERGY-v1 envelope");
	require((record.word(13) & ~energy_header_allowed_mask) == 0u,
		"ENERGY-v1 format header reserved bits are nonzero");
	require(record.energy_part() <= meter_energy_part_quadrants,
		"ENERGY-v1 part kind is invalid");
	require(record.energy_part_count() == meter_energy_part_count,
		"ENERGY-v1 part count is not two");
	require(record.energy_family_complete() && record.energy_complete(),
		"ENERGY-v1 family is not marked complete");
	require((record.status() & ~energy_status_allowed_mask) == 0u,
		"ENERGY-v1 status reserved bits are nonzero");
	require(record.emit_drops() == 0u && record.result_drops() == 0u,
		"ENERGY-v1 carries transport drops");
	require(record.block_sample_count() != 0u,
		"ENERGY-v1 source sample count is zero");
	require(record.energy_session_id() != 0u,
		"ENERGY-v1 session ID is zero");
	require(record.energy_last_sample_index() >= record.first_sample_index(),
		"ENERGY-v1 sample anchors are reversed");
	if (record.energy_part() == meter_energy_part_summary)
		require_zero_words(record, 40u, 47u,
			"ENERGY-v1 summary reserved words are nonzero");
	require_zero_words(record, 56u, 63u,
		"ENERGY-v1 tail reserved words are nonzero");

	if (record.energy_part() == meter_energy_part_summary) {
		for (const auto base : {meter_energy_summary_import_word,
			meter_energy_summary_export_word,
			meter_energy_summary_apparent_word})
			for (std::size_t index = 0; index < 4; ++index)
				(void)checked_counter(record, base + index * 2u);
	} else {
		for (const auto base : meter_energy_quadrant_words)
			for (std::size_t index = 0; index < 4; ++index)
				(void)checked_counter(record, base + index * 2u);
	}
}

void validate_demand(const MeterRecord &record)
{
	require(record.header_valid() &&
		record.record_format() == meter_demand_format,
		"invalid DEMAND-v1 envelope");
	require((record.word(13) & ~demand_header_allowed_mask) == 0u,
		"DEMAND-v1 format header reserved bits are nonzero");
	require(record.demand_interval_seconds() == meter_demand_interval_seconds,
		"DEMAND-v1 interval is not 600 seconds");
	require(record.demand_complete(), "DEMAND-v1 is not complete");
	require((record.status() & ~demand_status_allowed_mask) == 0u,
		"DEMAND-v1 status reserved bits are nonzero");
	require(record.emit_drops() == 0u && record.result_drops() == 0u,
		"DEMAND-v1 carries transport drops");
	require(record.block_sample_count() != 0u,
		"DEMAND-v1 source sample count is zero");
	require(record.demand_session_id() != 0u,
		"DEMAND-v1 session ID is zero");
	require(record.demand_last_sample_index() >= record.first_sample_index(),
		"DEMAND-v1 sample anchors are reversed");
	require_zero_words(record, 62u, 63u,
		"DEMAND-v1 tail reserved words are nonzero");
	for (std::size_t index = 0; index < 4; ++index) {
		(void)record.signed64(meter_demand_current_word + index * 2u);
		(void)checked_counter(record,
			meter_demand_import_peak_word + index * 2u);
		(void)checked_counter(record,
			meter_demand_export_peak_word + index * 2u);
	}
}

} // namespace

EnergyCounterArray flatten_energy_counters(const EnergyValues &values) noexcept
{
	EnergyCounterArray result{};
	const auto copy_group = [&result](const auto &group, std::size_t base) {
		result[base + 0] = group.phase_a.value;
		result[base + 1] = group.phase_b.value;
		result[base + 2] = group.phase_c.value;
		result[base + 3] = group.total.value;
	};
	copy_group(values.active_import, 0);
	copy_group(values.active_export, 4);
	copy_group(values.apparent, 8);
	for (std::size_t quadrant = 0; quadrant < 4; ++quadrant)
		copy_group(values.reactive_quadrants[quadrant], 12 + quadrant * 4);
	return result;
}

void assign_energy_counters(EnergyValues &values,
	const EnergyCounterArray &counters) noexcept
{
	const auto copy_group = [&counters](auto &group, std::size_t base) {
		group.phase_a.value = counters[base + 0];
		group.phase_b.value = counters[base + 1];
		group.phase_c.value = counters[base + 2];
		group.total.value = counters[base + 3];
	};
	copy_group(values.active_import, 0);
	copy_group(values.active_export, 4);
	copy_group(values.apparent, 8);
	for (std::size_t quadrant = 0; quadrant < 4; ++quadrant)
		copy_group(values.reactive_quadrants[quadrant], 12 + quadrant * 4);
}

DemandValueArray flatten_demand_values(
	const PhaseABCTotal<Reading<MicroWatts>> &values) noexcept
{
	return {values.phase_a.value, values.phase_b.value,
		values.phase_c.value, values.total.value};
}

void assign_demand_values(PhaseABCTotal<Reading<MicroWatts>> &values,
	const DemandValueArray &source) noexcept
{
	values.phase_a.value = source[0];
	values.phase_b.value = source[1];
	values.phase_c.value = source[2];
	values.total.value = source[3];
}

EnergyFamilyIdentity decode_energy_identity(const MeterRecord &record)
{
	validate_energy_part(record);
	return {
		record.sequence(),
		record.configuration_generation(),
		record.sample_rate_hz(),
		record.block_sample_count(),
		record.valid_mask(),
		record.status(),
		record.first_sample_index(),
		record.energy_last_sample_index(),
		record.energy_session_id(),
		record.energy_accepted_samples(),
		record.energy_skipped_samples(),
		record.energy_accepted_blocks(),
		record.energy_skipped_blocks(),
	};
}

EnergyValues decode_energy_family(const MeterRecord &summary,
	const MeterRecord &quadrants, SystemTime received_at)
{
	const auto summary_identity = decode_energy_identity(summary);
	const auto quadrant_identity = decode_energy_identity(quadrants);
	require(summary.energy_part() == meter_energy_part_summary,
		"ENERGY-v1 summary part is missing");
	require(quadrants.energy_part() == meter_energy_part_quadrants,
		"ENERGY-v1 quadrant part is missing");
	require(summary_identity == quadrant_identity,
		"ENERGY-v1 parts have mismatched family identity");
	const auto window = source_window(summary);
	const bool saturated = summary.energy_saturated();
	EnergyValues result;
	for (std::size_t index = 0; index < 4; ++index) {
		const bool summary_valid =
			(summary.energy_category_valid_mask() & (1u << index)) != 0u;
		const bool quadrant_valid =
			(quadrants.energy_category_valid_mask() & (1u << index)) != 0u;
		phase_total(result.active_import, index) =
			counter_reading<MicroWattHours>(summary,
				meter_energy_summary_import_word + index * 2u,
				summary_valid, saturated, received_at, window);
		phase_total(result.active_export, index) =
			counter_reading<MicroWattHours>(summary,
				meter_energy_summary_export_word + index * 2u,
				summary_valid, saturated, received_at, window);
		phase_total(result.apparent, index) =
			counter_reading<MicroVoltAmpereHours>(summary,
				meter_energy_summary_apparent_word + index * 2u,
				summary_valid, saturated, received_at, window);
		for (std::size_t quadrant = 0; quadrant < 4; ++quadrant)
			phase_total(result.reactive_quadrants[quadrant], index) =
				counter_reading<MicroVarHours>(quadrants,
					meter_energy_quadrant_words[quadrant] + index * 2u,
					quadrant_valid, saturated, received_at, window);
	}
	result.session_id = summary_identity.session_id;
	result.last_sample_index = summary_identity.last_sample_index;
	result.accepted_samples = summary_identity.accepted_samples;
	result.skipped_samples = summary_identity.skipped_samples;
	result.accepted_blocks = summary_identity.accepted_blocks;
	result.skipped_blocks = summary_identity.skipped_blocks;
	result.saturated = saturated;
	result.incomplete_input = summary.energy_incomplete_input();
	result.discontinuity = summary.energy_discontinuity();
	return result;
}

EnergyAssemblyUpdate EnergyFamilyAssembler::accept(const MeterRecord &record,
	SystemTime received_at)
{
	const auto incoming = decode_energy_identity(record);
	EnergyAssemblyUpdate update;
	if (identity_ && *identity_ != incoming) {
		if (summary_ || quadrants_)
			update.incomplete_families = 1;
		reset();
	}
	if (!identity_)
		identity_ = incoming;

	auto &slot = record.energy_part() == meter_energy_part_summary
		? summary_ : quadrants_;
	if (slot) {
		if (slot->words != record.words)
			throw std::invalid_argument(
				"ENERGY-v1 duplicate part has different payload");
		update.duplicate_part = true;
	} else {
		slot = record;
	}

	if (summary_ && quadrants_) {
		update.completed = decode_energy_family(*summary_, *quadrants_,
			received_at);
		reset();
	}
	return update;
}

void EnergyFamilyAssembler::reset() noexcept
{
	identity_.reset();
	summary_.reset();
	quadrants_.reset();
}

std::size_t EnergyFamilyAssembler::pending_parts() const noexcept
{
	return static_cast<std::size_t>(summary_.has_value()) +
		static_cast<std::size_t>(quadrants_.has_value());
}

MeterUpdate decode_demand_meter_record(const MeterRecord &record,
	SystemTime received_at)
{
	validate_demand(record);
	const auto window = source_window(record);
	const bool interval_valid = record.demand_time_aligned() &&
		record.demand_boundary_valid() && !record.demand_contaminated();
	DemandValues values;
	for (std::size_t index = 0; index < 4; ++index) {
		const bool valid = interval_valid &&
			(record.demand_valid_mask() & (1u << index)) != 0u;
		phase_total(values.current_active, index) = signed_demand_reading(
			record, meter_demand_current_word + index * 2u, valid,
			record.demand_saturated(), received_at, window);
		phase_total(values.import_peak, index) =
			counter_reading<MicroWatts>(record,
				meter_demand_import_peak_word + index * 2u, true,
				record.demand_saturated(), received_at, window);
		phase_total(values.export_peak, index) =
			counter_reading<MicroWatts>(record,
				meter_demand_export_peak_word + index * 2u, true,
				record.demand_saturated(), received_at, window);
		phase_total(values.import_peak_sample, index) = record.unsigned64(
			meter_demand_import_peak_anchor_word + index * 2u);
		phase_total(values.export_peak_sample, index) = record.unsigned64(
			meter_demand_export_peak_anchor_word + index * 2u);
	}
	values.session_id = record.demand_session_id();
	values.last_sample_index = record.demand_last_sample_index();
	values.interval_target_sample =
		record.unsigned64(meter_demand_target_sample_word);
	values.source_interval_count =
		record.word(meter_demand_source_interval_count_word);
	values.source_status = record.word(meter_demand_source_status_word);
	values.time_aligned = record.demand_time_aligned();
	values.contaminated = record.demand_contaminated();
	values.boundary_valid = record.demand_boundary_valid();
	values.saturated = record.demand_saturated();
	values.incomplete_input = record.demand_incomplete_input();

	MeterUpdate update;
	update.period = MeasurementPeriod::Min10;
	update.kind = RecordKind::demand;
	update.sequence = record.sequence();
	update.configuration_generation = record.configuration_generation();
	update.demand = std::move(values);
	meter::AggregateTiming timing;
	timing.sequence = record.sequence();
	timing.configuration_generation = record.configuration_generation();
	timing.first_sample_index = record.first_sample_index();
	timing.last_sample_index = record.demand_last_sample_index();
	timing.sample_count = record.block_sample_count();
	timing.sample_rate_hz = record.sample_rate_hz();
	timing.time_aligned = record.demand_time_aligned();
	timing.contaminated = record.demand_contaminated();
	timing.boundary_valid = record.demand_boundary_valid();
	timing.target_sample_index =
		record.unsigned64(meter_demand_target_sample_word);
	update.aggregate_timing = timing;
	return update;
}

} // namespace msap1
