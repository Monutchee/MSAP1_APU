#include "msap1/meter/MeterDataProvider/snapshot/in_process_meter_snapshot_provider.hpp"

#include <array>
#include <chrono>
#include <stdexcept>

namespace msap1::meter {

namespace {

using mnc::meter::MeterAttributeId;
using mnc::meter::MeterAttributeKey;
using mnc::meter::MeterAttributeSet;
using mnc::meter::MeterAttributeValue;
using mnc::meter::MeterUnit;
using mnc::meter::ReadingQuality;

constexpr std::array energy_attribute_groups{
	std::array{MeterAttributeId::ActiveImportEnergyA,
		MeterAttributeId::ActiveImportEnergyB,
		MeterAttributeId::ActiveImportEnergyC,
		MeterAttributeId::ActiveImportEnergyTotal},
	std::array{MeterAttributeId::ActiveExportEnergyA,
		MeterAttributeId::ActiveExportEnergyB,
		MeterAttributeId::ActiveExportEnergyC,
		MeterAttributeId::ActiveExportEnergyTotal},
	std::array{MeterAttributeId::ApparentEnergyA,
		MeterAttributeId::ApparentEnergyB,
		MeterAttributeId::ApparentEnergyC,
		MeterAttributeId::ApparentEnergyTotal},
	std::array{MeterAttributeId::ReactiveEnergyQuadrantIA,
		MeterAttributeId::ReactiveEnergyQuadrantIB,
		MeterAttributeId::ReactiveEnergyQuadrantIC,
		MeterAttributeId::ReactiveEnergyQuadrantITotal},
	std::array{MeterAttributeId::ReactiveEnergyQuadrantIIA,
		MeterAttributeId::ReactiveEnergyQuadrantIIB,
		MeterAttributeId::ReactiveEnergyQuadrantIIC,
		MeterAttributeId::ReactiveEnergyQuadrantIITotal},
	std::array{MeterAttributeId::ReactiveEnergyQuadrantIIIA,
		MeterAttributeId::ReactiveEnergyQuadrantIIIB,
		MeterAttributeId::ReactiveEnergyQuadrantIIIC,
		MeterAttributeId::ReactiveEnergyQuadrantIIITotal},
	std::array{MeterAttributeId::ReactiveEnergyQuadrantIVA,
		MeterAttributeId::ReactiveEnergyQuadrantIVB,
		MeterAttributeId::ReactiveEnergyQuadrantIVC,
		MeterAttributeId::ReactiveEnergyQuadrantIVTotal},
};

constexpr std::array demand_attribute_groups{
	std::array{MeterAttributeId::CurrentActiveDemandA,
		MeterAttributeId::CurrentActiveDemandB,
		MeterAttributeId::CurrentActiveDemandC,
		MeterAttributeId::CurrentActiveDemandTotal},
	std::array{MeterAttributeId::ImportDemandPeakA,
		MeterAttributeId::ImportDemandPeakB,
		MeterAttributeId::ImportDemandPeakC,
		MeterAttributeId::ImportDemandPeakTotal},
	std::array{MeterAttributeId::ExportDemandPeakA,
		MeterAttributeId::ExportDemandPeakB,
		MeterAttributeId::ExportDemandPeakC,
		MeterAttributeId::ExportDemandPeakTotal},
};

mnc::meter::TimeQuality time_quality(msap1::TimeQuality value)
{
	switch (value) {
	case msap1::TimeQuality::Synchronized:
		return mnc::meter::TimeQuality::Synchronized;
	case msap1::TimeQuality::Holdover:
		return mnc::meter::TimeQuality::Holdover;
	case msap1::TimeQuality::Unsynchronized:
		break;
	}
	return mnc::meter::TimeQuality::Unsynchronized;
}

template<typename Timing>
std::optional<mnc::meter::MeterSnapshotTiming> snapshot_timing(
	const Timing &timing)
{
	mnc::meter::MeterSnapshotTiming result{};
	result.quality = time_quality(timing.time_quality);
	result.first_sample_index = timing.first_sample_index;
	result.sample_count = timing.sample_count;
	result.sample_rate_hz = timing.sample_rate_hz;
	result.cycle_count = timing.cycle_count;
	result.nominal_frequency_hz =
		static_cast<std::uint32_t>(timing.nominal_frequency);
	if constexpr (requires {
		timing.basic_block_count;
		timing.first_basic_sequence;
		timing.last_basic_sequence;
		timing.target_sample_index;
		timing.overshoot_samples;
		timing.time_aligned;
		timing.contaminated;
		timing.boundary_valid;
	}) {
		result.source_interval_count = timing.basic_block_count;
		result.first_source_sequence = timing.first_basic_sequence;
		result.last_source_sequence = timing.last_basic_sequence;
		result.expected_end_sample_index = timing.target_sample_index;
		result.overshoot_samples = timing.overshoot_samples;
		result.time_aligned = timing.time_aligned;
		result.contaminated = timing.contaminated;
		result.boundary_valid = timing.boundary_valid;
	}
	if (timing.utc_start) {
		result.utc_start_nanoseconds =
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				timing.utc_start->time_since_epoch()).count();
		result.utc_uncertainty_nanoseconds = timing.utc_uncertainty_ns;
	}
	return result;
}

ReadingQuality quality(msap1::MeasurementQuality value)
{
	switch (value) {
	case msap1::MeasurementQuality::unavailable:
		return ReadingQuality::Unavailable;
	case msap1::MeasurementQuality::valid:
		return ReadingQuality::Valid;
	case msap1::MeasurementQuality::invalid:
		return ReadingQuality::Invalid;
	case msap1::MeasurementQuality::out_of_range:
		return ReadingQuality::OutOfRange;
	case msap1::MeasurementQuality::timed_out:
		return ReadingQuality::TimedOut;
	case msap1::MeasurementQuality::arithmetic_error:
		return ReadingQuality::ArithmeticError;
	}
	return ReadingQuality::Unavailable;
}

template<typename Unit>
MeterAttributeValue value(MeterAttributeKey attribute, MeterUnit unit,
			  const msap1::Reading<Unit> &reading)
{
	return {
		attribute,
		unit,
		quality(reading.quality),
		reading.value,
		reading.source_sequence,
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			reading.measured_at.time_since_epoch()).count(),
		reading.calculation_window.sample_count,
		reading.calculation_window.duration.count(),
	};
}

template<typename Unit>
MeterAttributeValue categorical_value(MeterAttributeKey attribute,
	std::int64_t categorical, const msap1::Reading<Unit> &provenance)
{
	auto result = value(attribute, MeterUnit::CategoricalCode, provenance);
	result.value = categorical;
	return result;
}

MeterAttributeValue unavailable(MeterAttributeKey attribute)
{
	const auto descriptor = mnc::meter::describe(attribute);
	return {attribute, descriptor.unit, ReadingQuality::Unavailable};
}

template<typename Unit>
bool append_group(std::vector<MeterAttributeValue> &output,
	MeterAttributeKey attribute, const std::array<MeterAttributeId, 4> &ids,
	MeterUnit unit, const PhaseABCTotal<Reading<Unit>> &group)
{
	const std::array<const Reading<Unit> *, 4> readings{
		&group.phase_a, &group.phase_b, &group.phase_c, &group.total};
	for (std::size_t index = 0; index < ids.size(); ++index) {
		if (attribute.id == ids[index]) {
			output.push_back(value(attribute, unit, *readings[index]));
			return true;
		}
	}
	return false;
}

template<typename Unit>
void replace_group(std::vector<MeterAttributeValue> &output,
	const std::array<MeterAttributeId, 4> &ids, MeterUnit unit,
	const PhaseABCTotal<Reading<Unit>> &group)
{
	const std::array<const Reading<Unit> *, 4> readings{
		&group.phase_a, &group.phase_b, &group.phase_c, &group.total};
	for (auto &attribute : output) {
		for (std::size_t index = 0; index < ids.size(); ++index) {
			if (attribute.attribute.id == ids[index]) {
				attribute = value(attribute.attribute, unit, *readings[index]);
				break;
			}
		}
	}
}

std::vector<MeterAttributeKey> supported(msap1::MeasurementPeriod period)
{
	return mnc::meter::attributes_for(period,
		mnc::meter::MeterAttributeUsage::Snapshot);
}

} // namespace

void overlay_authoritative_energy(mnc::meter::MeterSnapshot &snapshot,
	const msap1::EnergyValues &energy)
{
	snapshot.energy = mnc::meter::EnergySnapshotMetadata{
		energy.session_id, energy.reset_epoch, energy.last_sample_index,
		energy.accepted_samples, energy.skipped_samples,
		energy.accepted_blocks, energy.skipped_blocks,
		energy.saturated, energy.incomplete_input, energy.discontinuity};
	replace_group(snapshot.values, energy_attribute_groups[0],
		MeterUnit::MicroWattHours, energy.active_import);
	replace_group(snapshot.values, energy_attribute_groups[1],
		MeterUnit::MicroWattHours, energy.active_export);
	replace_group(snapshot.values, energy_attribute_groups[2],
		MeterUnit::MicroVoltAmpereHours, energy.apparent);
	for (std::size_t quadrant = 0; quadrant < energy.reactive_quadrants.size();
	     ++quadrant)
		replace_group(snapshot.values, energy_attribute_groups[3 + quadrant],
			MeterUnit::MicroVarHours, energy.reactive_quadrants[quadrant]);
}

void overlay_authoritative_demand(mnc::meter::MeterSnapshot &snapshot,
	const msap1::DemandValues &demand)
{
	snapshot.demand = mnc::meter::DemandSnapshotMetadata{
		.session_id = demand.session_id,
		.peak_reset_epoch = demand.peak_reset_epoch,
		.last_sample_index = demand.last_sample_index,
		.interval_anchor_sample = demand.interval_anchor_sample,
		.source_interval_count = demand.source_interval_count,
		.source_status = demand.source_status,
		.window_seconds = demand.window_seconds,
		.update_seconds = demand.update_seconds,
		.profile_generation = demand.profile_generation,
		.method = static_cast<std::uint8_t>(demand.method),
		.import_peak_samples = {demand.import_peak_sample.phase_a,
			demand.import_peak_sample.phase_b,
			demand.import_peak_sample.phase_c,
			demand.import_peak_sample.total},
		.export_peak_samples = {demand.export_peak_sample.phase_a,
			demand.export_peak_sample.phase_b,
			demand.export_peak_sample.phase_c,
			demand.export_peak_sample.total},
		.time_aligned = demand.time_aligned,
		.contaminated = demand.contaminated,
		.boundary_valid = demand.boundary_valid,
		.saturated = demand.saturated,
		.incomplete_input = demand.incomplete_input,
	};
	replace_group(snapshot.values, demand_attribute_groups[0],
		MeterUnit::MicroWatts, demand.current_active);
	replace_group(snapshot.values, demand_attribute_groups[1],
		MeterUnit::MicroWatts, demand.import_peak);
	replace_group(snapshot.values, demand_attribute_groups[2],
		MeterUnit::MicroWatts, demand.export_peak);
}

std::vector<mnc::meter::MeterCapabilities>
InProcessMeterSnapshotProvider::capabilities() const
{
	return {
		{MeasurementPeriod::Basic, supported(MeasurementPeriod::Basic)},
		{MeasurementPeriod::Cycles150_180,
		 supported(MeasurementPeriod::Cycles150_180)},
		{MeasurementPeriod::Min10, supported(MeasurementPeriod::Min10)},
		{MeasurementPeriod::Hour2, supported(MeasurementPeriod::Hour2)},
		{MeasurementPeriod::Min10Live,
		 supported(MeasurementPeriod::Min10Live)},
		{MeasurementPeriod::Hour2Live,
		 supported(MeasurementPeriod::Hour2Live)},
		{MeasurementPeriod::Demand, supported(MeasurementPeriod::Demand)},
	};
}

mnc::meter::MeterSnapshot InProcessMeterSnapshotProvider::project(
	const msap1::MeterPeriodView &view,
	const mnc::meter::MeterSnapshotRequest &request)
{
	MeterAttributeSet selection(request.attributes);
	if (selection.empty())
		selection = MeterAttributeSet(supported(view.period));

	mnc::meter::MeterSnapshot result{};
	result.period = view.period;
	result.sequence = view.latest_sequence;
	result.configuration_generation = view.configuration_generation;
	result.updated_at_nanoseconds =
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			view.updated_at.time_since_epoch()).count();
	/* Timing is copied from the immutable view populated by the record
	 * ingestor. It is measurement provenance, never reconstructed from the
	 * current clock at request time. */
	if (view.timing)
		result.timing = snapshot_timing(*view.timing);
	else if (view.aggregate_timing)
		result.timing = snapshot_timing(*view.aggregate_timing);
	if (view.period == MeasurementPeriod::Basic &&
	    view.values.energy.session_id != 0) {
		const auto &energy = view.values.energy;
		result.energy = mnc::meter::EnergySnapshotMetadata{
			energy.session_id, energy.reset_epoch, energy.last_sample_index,
			energy.accepted_samples, energy.skipped_samples,
			energy.accepted_blocks, energy.skipped_blocks,
			energy.saturated, energy.incomplete_input, energy.discontinuity};
	}
	if (view.period == MeasurementPeriod::Demand &&
	    view.values.demand.session_id != 0) {
		const auto &demand = view.values.demand;
		result.demand = mnc::meter::DemandSnapshotMetadata{
			.session_id = demand.session_id,
			.peak_reset_epoch = demand.peak_reset_epoch,
			.last_sample_index = demand.last_sample_index,
			.interval_anchor_sample = demand.interval_anchor_sample,
			.source_interval_count = demand.source_interval_count,
			.source_status = demand.source_status,
			.window_seconds = demand.window_seconds,
			.update_seconds = demand.update_seconds,
			.profile_generation = demand.profile_generation,
			.method = static_cast<std::uint8_t>(demand.method),
			.import_peak_samples = {demand.import_peak_sample.phase_a,
				demand.import_peak_sample.phase_b,
				demand.import_peak_sample.phase_c,
				demand.import_peak_sample.total},
			.export_peak_samples = {demand.export_peak_sample.phase_a,
				demand.export_peak_sample.phase_b,
				demand.export_peak_sample.phase_c,
				demand.export_peak_sample.total},
			.time_aligned = demand.time_aligned,
			.contaminated = demand.contaminated,
			.boundary_valid = demand.boundary_valid,
			.saturated = demand.saturated,
			.incomplete_input = demand.incomplete_input,
		};
	}

	for (const auto attribute : selection.values()) {
		/* Validate the canonical identity before interpreting provider
		 * support. Known catalog entries that this provider cannot produce
		 * become Unavailable; malformed enum values remain programmer or
		 * protocol errors and must not disappear silently. */
		(void)mnc::meter::describe(attribute);
		/* Indexed attributes are reserved for future families such as
		 * harmonic order.  The current PL records provide fundamentals
		 * only, so an indexed key must not alias the unindexed reading. */
		if (attribute.index) {
			result.values.push_back(unavailable(attribute));
			continue;
		}
		const auto &energy = view.values.energy;
		if (append_group(result.values, attribute, energy_attribute_groups[0],
				MeterUnit::MicroWattHours, energy.active_import) ||
		    append_group(result.values, attribute, energy_attribute_groups[1],
				MeterUnit::MicroWattHours, energy.active_export) ||
		    append_group(result.values, attribute, energy_attribute_groups[2],
				MeterUnit::MicroVoltAmpereHours, energy.apparent) ||
		    append_group(result.values, attribute, energy_attribute_groups[3],
				MeterUnit::MicroVarHours, energy.reactive_quadrants[0]) ||
		    append_group(result.values, attribute, energy_attribute_groups[4],
				MeterUnit::MicroVarHours, energy.reactive_quadrants[1]) ||
		    append_group(result.values, attribute, energy_attribute_groups[5],
				MeterUnit::MicroVarHours, energy.reactive_quadrants[2]) ||
		    append_group(result.values, attribute, energy_attribute_groups[6],
				MeterUnit::MicroVarHours, energy.reactive_quadrants[3]))
			continue;
		const auto &demand = view.values.demand;
		if (append_group(result.values, attribute, demand_attribute_groups[0],
				MeterUnit::MicroWatts, demand.current_active) ||
		    append_group(result.values, attribute, demand_attribute_groups[1],
				MeterUnit::MicroWatts, demand.import_peak) ||
		    append_group(result.values, attribute, demand_attribute_groups[2],
				MeterUnit::MicroWatts, demand.export_peak))
			continue;
		switch (attribute.id) {
		case MeterAttributeId::Frequency:
			result.values.push_back(value(attribute, MeterUnit::MilliHertz,
				view.values.fundamental.frequency));
			break;
		case MeterAttributeId::VanRms:
			result.values.push_back(value(attribute, MeterUnit::MicroVolts,
				view.values.fundamental.voltage_ln.phase_a));
			break;
		case MeterAttributeId::VbnRms:
			result.values.push_back(value(attribute, MeterUnit::MicroVolts,
				view.values.fundamental.voltage_ln.phase_b));
			break;
		case MeterAttributeId::VcnRms:
			result.values.push_back(value(attribute, MeterUnit::MicroVolts,
				view.values.fundamental.voltage_ln.phase_c));
			break;
		case MeterAttributeId::IaRms:
			result.values.push_back(value(attribute, MeterUnit::MicroAmperes,
				view.values.fundamental.current.phase_a));
			break;
		case MeterAttributeId::IbRms:
			result.values.push_back(value(attribute, MeterUnit::MicroAmperes,
				view.values.fundamental.current.phase_b));
			break;
		case MeterAttributeId::IcRms:
			result.values.push_back(value(attribute, MeterUnit::MicroAmperes,
				view.values.fundamental.current.phase_c));
			break;
		case MeterAttributeId::InRms:
			result.values.push_back(value(attribute, MeterUnit::MicroAmperes,
				view.values.fundamental.current.neutral));
			break;
		case MeterAttributeId::VabRms:
			result.values.push_back(value(attribute, MeterUnit::MicroVolts,
				view.values.fundamental.voltage_ll.phase_a));
			break;
		case MeterAttributeId::VbcRms:
			result.values.push_back(value(attribute, MeterUnit::MicroVolts,
				view.values.fundamental.voltage_ll.phase_b));
			break;
		case MeterAttributeId::VcaRms:
			result.values.push_back(value(attribute, MeterUnit::MicroVolts,
				view.values.fundamental.voltage_ll.phase_c));
			break;
		case MeterAttributeId::ActivePowerA:
			result.values.push_back(value(attribute, MeterUnit::Picowatts,
				view.values.power.active_power.phase_a));
			break;
		case MeterAttributeId::ActivePowerB:
			result.values.push_back(value(attribute, MeterUnit::Picowatts,
				view.values.power.active_power.phase_b));
			break;
		case MeterAttributeId::ActivePowerC:
			result.values.push_back(value(attribute, MeterUnit::Picowatts,
				view.values.power.active_power.phase_c));
			break;
		case MeterAttributeId::ActivePowerTotal:
			result.values.push_back(value(attribute, MeterUnit::Picowatts,
				view.values.power.total_active_power));
			break;
		case MeterAttributeId::ApparentPowerA:
			result.values.push_back(value(attribute,
				MeterUnit::PicoVoltAmperes,
				view.values.power.apparent_power.phase_a));
			break;
		case MeterAttributeId::ApparentPowerB:
			result.values.push_back(value(attribute,
				MeterUnit::PicoVoltAmperes,
				view.values.power.apparent_power.phase_b));
			break;
		case MeterAttributeId::ApparentPowerC:
			result.values.push_back(value(attribute,
				MeterUnit::PicoVoltAmperes,
				view.values.power.apparent_power.phase_c));
			break;
		case MeterAttributeId::ApparentPowerTotal:
			result.values.push_back(value(attribute,
				MeterUnit::PicoVoltAmperes,
				view.values.power.total_apparent_power));
			break;
		case MeterAttributeId::PowerFactorA:
			result.values.push_back(value(attribute,
				MeterUnit::PowerFactorMillionths,
				view.values.power.power_factor.phase_a));
			break;
		case MeterAttributeId::PowerFactorB:
			result.values.push_back(value(attribute,
				MeterUnit::PowerFactorMillionths,
				view.values.power.power_factor.phase_b));
			break;
		case MeterAttributeId::PowerFactorC:
			result.values.push_back(value(attribute,
				MeterUnit::PowerFactorMillionths,
				view.values.power.power_factor.phase_c));
			break;
		case MeterAttributeId::PowerFactorTotal:
			result.values.push_back(value(attribute,
				MeterUnit::PowerFactorMillionths,
				view.values.power.total_power_factor));
			break;
		case MeterAttributeId::ReactivePowerA:
			result.values.push_back(value(attribute, MeterUnit::Picovars,
				view.values.phasor.reactive_power.phase_a));
			break;
		case MeterAttributeId::ReactivePowerB:
			result.values.push_back(value(attribute, MeterUnit::Picovars,
				view.values.phasor.reactive_power.phase_b));
			break;
		case MeterAttributeId::ReactivePowerC:
			result.values.push_back(value(attribute, MeterUnit::Picovars,
				view.values.phasor.reactive_power.phase_c));
			break;
		case MeterAttributeId::ReactivePowerTotal:
			result.values.push_back(value(attribute, MeterUnit::Picovars,
				view.values.phasor.total_reactive_power));
			break;
		case MeterAttributeId::DisplacementPowerFactorA:
			result.values.push_back(value(attribute,
				MeterUnit::PowerFactorMillionths,
				view.values.phasor.displacement_power_factor.phase_a));
			break;
		case MeterAttributeId::DisplacementPowerFactorB:
			result.values.push_back(value(attribute,
				MeterUnit::PowerFactorMillionths,
				view.values.phasor.displacement_power_factor.phase_b));
			break;
		case MeterAttributeId::DisplacementPowerFactorC:
			result.values.push_back(value(attribute,
				MeterUnit::PowerFactorMillionths,
				view.values.phasor.displacement_power_factor.phase_c));
			break;
		case MeterAttributeId::DisplacementPowerFactorTotal:
			result.values.push_back(value(attribute,
				MeterUnit::PowerFactorMillionths,
				view.values.phasor.total_displacement_power_factor));
			break;
		case MeterAttributeId::VoltagePhaseAngleA:
			result.values.push_back(value(attribute,
				MeterUnit::Millidegrees,
				view.values.phasor.voltage_angle.phase_a));
			break;
		case MeterAttributeId::VoltagePhaseAngleB:
			result.values.push_back(value(attribute,
				MeterUnit::Millidegrees,
				view.values.phasor.voltage_angle.phase_b));
			break;
		case MeterAttributeId::VoltagePhaseAngleC:
			result.values.push_back(value(attribute,
				MeterUnit::Millidegrees,
				view.values.phasor.voltage_angle.phase_c));
			break;
		case MeterAttributeId::CurrentPhaseAngleA:
			result.values.push_back(value(attribute,
				MeterUnit::Millidegrees,
				view.values.phasor.current_angle.phase_a));
			break;
		case MeterAttributeId::CurrentPhaseAngleB:
			result.values.push_back(value(attribute,
				MeterUnit::Millidegrees,
				view.values.phasor.current_angle.phase_b));
			break;
		case MeterAttributeId::CurrentPhaseAngleC:
			result.values.push_back(value(attribute,
				MeterUnit::Millidegrees,
				view.values.phasor.current_angle.phase_c));
			break;
		case MeterAttributeId::VoltageUnbalance:
			result.values.push_back(value(attribute,
				MeterUnit::RatioMillionths,
				view.values.unbalance.voltage_unbalance));
			break;
		case MeterAttributeId::CurrentUnbalance:
			result.values.push_back(value(attribute,
				MeterUnit::RatioMillionths,
				view.values.unbalance.current_unbalance));
			break;
		case MeterAttributeId::VoltageZeroSequenceRatio:
			result.values.push_back(value(attribute,
				MeterUnit::RatioMillionths,
				view.values.unbalance.voltage_zero_ratio));
			break;
		case MeterAttributeId::CurrentZeroSequenceRatio:
			result.values.push_back(value(attribute,
				MeterUnit::RatioMillionths,
				view.values.unbalance.current_zero_ratio));
			break;
		case MeterAttributeId::ZeroSequenceVoltage:
			result.values.push_back(value(attribute,
				MeterUnit::MicroVolts,
				view.values.unbalance.voltage_zero_sequence));
			break;
		case MeterAttributeId::PositiveSequenceVoltage:
			result.values.push_back(value(attribute,
				MeterUnit::MicroVolts,
				view.values.unbalance.voltage_positive_sequence));
			break;
		case MeterAttributeId::NegativeSequenceVoltage:
			result.values.push_back(value(attribute,
				MeterUnit::MicroVolts,
				view.values.unbalance.voltage_negative_sequence));
			break;
		case MeterAttributeId::ZeroSequenceCurrent:
			result.values.push_back(value(attribute,
				MeterUnit::MicroAmperes,
				view.values.unbalance.current_zero_sequence));
			break;
		case MeterAttributeId::PositiveSequenceCurrent:
			result.values.push_back(value(attribute,
				MeterUnit::MicroAmperes,
				view.values.unbalance.current_positive_sequence));
			break;
		case MeterAttributeId::NegativeSequenceCurrent:
			result.values.push_back(value(attribute,
				MeterUnit::MicroAmperes,
				view.values.unbalance.current_negative_sequence));
			break;
		case MeterAttributeId::FundamentalVoltageA:
			result.values.push_back(value(attribute, MeterUnit::MicroVolts,
				view.values.phasor.fundamental_voltage.phase_a));
			break;
		case MeterAttributeId::FundamentalVoltageB:
			result.values.push_back(value(attribute, MeterUnit::MicroVolts,
				view.values.phasor.fundamental_voltage.phase_b));
			break;
		case MeterAttributeId::FundamentalVoltageC:
			result.values.push_back(value(attribute, MeterUnit::MicroVolts,
				view.values.phasor.fundamental_voltage.phase_c));
			break;
		case MeterAttributeId::FundamentalVoltageLlAB:
			result.values.push_back(value(attribute, MeterUnit::MicroVolts,
				view.values.phasor.fundamental_voltage_ll.phase_a));
			break;
		case MeterAttributeId::FundamentalVoltageLlBC:
			result.values.push_back(value(attribute, MeterUnit::MicroVolts,
				view.values.phasor.fundamental_voltage_ll.phase_b));
			break;
		case MeterAttributeId::FundamentalVoltageLlCA:
			result.values.push_back(value(attribute, MeterUnit::MicroVolts,
				view.values.phasor.fundamental_voltage_ll.phase_c));
			break;
		case MeterAttributeId::FundamentalCurrentA:
			result.values.push_back(value(attribute, MeterUnit::MicroAmperes,
				view.values.phasor.fundamental_current.phase_a));
			break;
		case MeterAttributeId::FundamentalCurrentB:
			result.values.push_back(value(attribute, MeterUnit::MicroAmperes,
				view.values.phasor.fundamental_current.phase_b));
			break;
		case MeterAttributeId::FundamentalCurrentC:
			result.values.push_back(value(attribute, MeterUnit::MicroAmperes,
				view.values.phasor.fundamental_current.phase_c));
			break;
		case MeterAttributeId::FundamentalCurrentN:
			result.values.push_back(value(attribute, MeterUnit::MicroAmperes,
				view.values.phasor.fundamental_current.neutral));
			break;
		case MeterAttributeId::VoltageCrestA:
			result.values.push_back(value(attribute,
				MeterUnit::CrestTenThousandths,
				view.values.power.voltage_crest.phase_a));
			break;
		case MeterAttributeId::VoltageCrestB:
			result.values.push_back(value(attribute,
				MeterUnit::CrestTenThousandths,
				view.values.power.voltage_crest.phase_b));
			break;
		case MeterAttributeId::VoltageCrestC:
			result.values.push_back(value(attribute,
				MeterUnit::CrestTenThousandths,
				view.values.power.voltage_crest.phase_c));
			break;
		case MeterAttributeId::CurrentCrestA:
			result.values.push_back(value(attribute,
				MeterUnit::CrestTenThousandths,
				view.values.power.current_crest.phase_a));
			break;
		case MeterAttributeId::CurrentCrestB:
			result.values.push_back(value(attribute,
				MeterUnit::CrestTenThousandths,
				view.values.power.current_crest.phase_b));
			break;
		case MeterAttributeId::CurrentCrestC:
			result.values.push_back(value(attribute,
				MeterUnit::CrestTenThousandths,
				view.values.power.current_crest.phase_c));
			break;
		case MeterAttributeId::CurrentCrestN:
			result.values.push_back(value(attribute,
				MeterUnit::CrestTenThousandths,
				view.values.power.current_crest.neutral));
			break;
		case MeterAttributeId::FundamentalActivePowerA:
			result.values.push_back(value(attribute, MeterUnit::Picowatts,
				view.values.phasor.fundamental_active_power.phase_a));
			break;
		case MeterAttributeId::FundamentalActivePowerB:
			result.values.push_back(value(attribute, MeterUnit::Picowatts,
				view.values.phasor.fundamental_active_power.phase_b));
			break;
		case MeterAttributeId::FundamentalActivePowerC:
			result.values.push_back(value(attribute, MeterUnit::Picowatts,
				view.values.phasor.fundamental_active_power.phase_c));
			break;
		case MeterAttributeId::FundamentalActivePowerTotal:
			result.values.push_back(value(attribute, MeterUnit::Picowatts,
				view.values.phasor.total_fundamental_active_power));
			break;
		case MeterAttributeId::CurrentPhaseAngleN:
			result.values.push_back(value(attribute, MeterUnit::Millidegrees,
				view.values.phasor.current_angle.neutral));
			break;
		case MeterAttributeId::VoltageLlPhaseAngleAB:
			result.values.push_back(value(attribute, MeterUnit::Millidegrees,
				view.values.phasor.voltage_ll_angle.phase_a));
			break;
		case MeterAttributeId::VoltageLlPhaseAngleBC:
			result.values.push_back(value(attribute, MeterUnit::Millidegrees,
				view.values.phasor.voltage_ll_angle.phase_b));
			break;
		case MeterAttributeId::VoltageLlPhaseAngleCA:
			result.values.push_back(value(attribute, MeterUnit::Millidegrees,
				view.values.phasor.voltage_ll_angle.phase_c));
			break;
		case MeterAttributeId::DisplacementAngleA:
			result.values.push_back(value(attribute, MeterUnit::Millidegrees,
				view.values.phasor.displacement_angle.phase_a));
			break;
		case MeterAttributeId::DisplacementAngleB:
			result.values.push_back(value(attribute, MeterUnit::Millidegrees,
				view.values.phasor.displacement_angle.phase_b));
			break;
		case MeterAttributeId::DisplacementAngleC:
			result.values.push_back(value(attribute, MeterUnit::Millidegrees,
				view.values.phasor.displacement_angle.phase_c));
			break;
		case MeterAttributeId::VoltageZeroSequenceAngle:
			result.values.push_back(value(attribute, MeterUnit::Millidegrees,
				view.values.unbalance.voltage_zero_angle));
			break;
		case MeterAttributeId::VoltagePositiveSequenceAngle:
			result.values.push_back(value(attribute, MeterUnit::Millidegrees,
				view.values.unbalance.voltage_positive_angle));
			break;
		case MeterAttributeId::VoltageNegativeSequenceAngle:
			result.values.push_back(value(attribute, MeterUnit::Millidegrees,
				view.values.unbalance.voltage_negative_angle));
			break;
		case MeterAttributeId::CurrentZeroSequenceAngle:
			result.values.push_back(value(attribute, MeterUnit::Millidegrees,
				view.values.unbalance.current_zero_angle));
			break;
		case MeterAttributeId::CurrentPositiveSequenceAngle:
			result.values.push_back(value(attribute, MeterUnit::Millidegrees,
				view.values.unbalance.current_positive_angle));
			break;
		case MeterAttributeId::CurrentNegativeSequenceAngle:
			result.values.push_back(value(attribute, MeterUnit::Millidegrees,
				view.values.unbalance.current_negative_angle));
			break;
		case MeterAttributeId::LoadNatureA:
			result.values.push_back(categorical_value(attribute,
				static_cast<std::int64_t>(view.values.phasor.load_nature.phase_a),
				view.values.phasor.displacement_power_factor.phase_a));
			break;
		case MeterAttributeId::LoadNatureB:
			result.values.push_back(categorical_value(attribute,
				static_cast<std::int64_t>(view.values.phasor.load_nature.phase_b),
				view.values.phasor.displacement_power_factor.phase_b));
			break;
		case MeterAttributeId::LoadNatureC:
			result.values.push_back(categorical_value(attribute,
				static_cast<std::int64_t>(view.values.phasor.load_nature.phase_c),
				view.values.phasor.displacement_power_factor.phase_c));
			break;
		case MeterAttributeId::LoadNatureTotal:
			result.values.push_back(categorical_value(attribute,
				static_cast<std::int64_t>(view.values.phasor.total_load_nature),
				view.values.phasor.total_displacement_power_factor));
			break;
		default:
			/* Known M17 groups were projected above. */
			break;
		}
	}
	return result;
}

std::optional<mnc::meter::MeterSnapshot>
InProcessMeterSnapshotProvider::latest(
	const mnc::meter::MeterSnapshotRequest &request) const
{
	const auto view = data_.latest(request.period);
	if (!view)
		return std::nullopt;
	return project(*view, request);
}

mnc::meter::LatestSubscription
InProcessMeterSnapshotProvider::subscribe_latest(
	const mnc::meter::MeterSnapshotRequest &request, Callback callback)
{
	if (!callback)
		throw std::invalid_argument("meter snapshot callback is empty");
	struct Owner {
		msap1::MeterData::Subscription subscription;
	};
	auto owner = std::make_shared<Owner>(Owner{data_.subscribe(
		request.period,
		[request, callback = std::move(callback)](
			const msap1::MeterPeriodView &view) {
			callback(project(view, request));
		})});
	return mnc::meter::LatestSubscription{std::move(owner)};
}

} // namespace msap1::meter
