#include "msap1/meter/MeterDataProvider/snapshot/in_process_meter_snapshot_provider.hpp"

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
	result.cycle_count = timing.cycle_count;
	result.nominal_frequency_hz =
		static_cast<std::uint32_t>(timing.nominal_frequency);
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

MeterAttributeValue unavailable(MeterAttributeKey attribute)
{
	const auto descriptor = mnc::meter::describe(attribute);
	return {attribute, descriptor.unit, ReadingQuality::Unavailable};
}

std::vector<MeterAttributeKey> supported(msap1::MeasurementPeriod period)
{
	using Id = MeterAttributeId;
	if (period == msap1::MeasurementPeriod::Min10 ||
	    period == msap1::MeasurementPeriod::Hour2)
		return {};
	std::vector<MeterAttributeKey> result{
		{Id::VanRms, std::nullopt}, {Id::VbnRms, std::nullopt},
		{Id::VcnRms, std::nullopt}, {Id::IaRms, std::nullopt},
		{Id::IbRms, std::nullopt}, {Id::IcRms, std::nullopt},
		{Id::InRms, std::nullopt},
	};
	if (period == msap1::MeasurementPeriod::Basic) {
		result.insert(result.begin(), MeterAttributeKey{Id::Frequency,
							       std::nullopt});
		/* The basic tier carries line-line RMS (BASIC-v4), the
		 * finalized power quantities (POWER-v1) and the fundamental
		 * phasor quantities (PHASOR-v1) since M7/M8/M9. The
		 * aggregate tier gains them in M11. */
		for (const auto id : {Id::VabRms, Id::VbcRms, Id::VcaRms,
				      Id::ActivePowerA, Id::ActivePowerB,
				      Id::ActivePowerC, Id::ActivePowerTotal,
				      Id::ApparentPowerA, Id::ApparentPowerB,
				      Id::ApparentPowerC,
				      Id::ApparentPowerTotal,
				      Id::PowerFactorA, Id::PowerFactorB,
				      Id::PowerFactorC, Id::PowerFactorTotal,
				      Id::ReactivePowerA, Id::ReactivePowerB,
				      Id::ReactivePowerC,
				      Id::ReactivePowerTotal,
				      Id::DisplacementPowerFactorA,
				      Id::DisplacementPowerFactorB,
				      Id::DisplacementPowerFactorC,
				      Id::DisplacementPowerFactorTotal,
				      Id::VoltagePhaseAngleA,
				      Id::VoltagePhaseAngleB,
				      Id::VoltagePhaseAngleC,
				      Id::CurrentPhaseAngleA,
				      Id::CurrentPhaseAngleB,
				      Id::CurrentPhaseAngleC})
			result.push_back({id, std::nullopt});
	}
	return result;
}

} // namespace

std::vector<mnc::meter::MeterCapabilities>
InProcessMeterSnapshotProvider::capabilities() const
{
	return {
		{MeasurementPeriod::Basic, supported(MeasurementPeriod::Basic)},
		{MeasurementPeriod::Cycles150_180,
		 supported(MeasurementPeriod::Cycles150_180)},
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
