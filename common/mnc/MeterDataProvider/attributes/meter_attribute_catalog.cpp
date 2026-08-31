#include "mnc/MeterDataProvider/attributes/meter_attribute_set.hpp"

#include <stdexcept>
#include <array>

namespace mnc::meter {

namespace {

using Id = MeterAttributeId;

constexpr MeterAttributeKey key(Id id)
{
	return MeterAttributeKey{id, std::nullopt};
}

constexpr std::array catalog{
	key(Id::Frequency), key(Id::VanRms), key(Id::VbnRms),
	key(Id::VcnRms), key(Id::VabRms), key(Id::VbcRms),
	key(Id::VcaRms), key(Id::IaRms), key(Id::IbRms),
	key(Id::IcRms), key(Id::InRms),
	key(Id::ActivePowerA), key(Id::ActivePowerB), key(Id::ActivePowerC),
	key(Id::ActivePowerTotal),
	key(Id::ApparentPowerA), key(Id::ApparentPowerB),
	key(Id::ApparentPowerC), key(Id::ApparentPowerTotal),
	key(Id::PowerFactorA), key(Id::PowerFactorB), key(Id::PowerFactorC),
	key(Id::PowerFactorTotal),
	key(Id::ReactivePowerA), key(Id::ReactivePowerB),
	key(Id::ReactivePowerC), key(Id::ReactivePowerTotal),
	key(Id::DisplacementPowerFactorA), key(Id::DisplacementPowerFactorB),
	key(Id::DisplacementPowerFactorC), key(Id::DisplacementPowerFactorTotal),
	key(Id::VoltagePhaseAngleA), key(Id::VoltagePhaseAngleB),
	key(Id::VoltagePhaseAngleC),
	key(Id::CurrentPhaseAngleA), key(Id::CurrentPhaseAngleB),
	key(Id::CurrentPhaseAngleC),
	key(Id::VoltageUnbalance), key(Id::CurrentUnbalance),
	key(Id::VoltageZeroSequenceRatio), key(Id::CurrentZeroSequenceRatio),
	key(Id::ZeroSequenceVoltage), key(Id::PositiveSequenceVoltage),
	key(Id::NegativeSequenceVoltage),
	key(Id::ZeroSequenceCurrent), key(Id::PositiveSequenceCurrent),
	key(Id::NegativeSequenceCurrent),
	key(Id::ActiveImportEnergyA), key(Id::ActiveImportEnergyB),
	key(Id::ActiveImportEnergyC), key(Id::ActiveImportEnergyTotal),
	key(Id::ActiveExportEnergyA), key(Id::ActiveExportEnergyB),
	key(Id::ActiveExportEnergyC), key(Id::ActiveExportEnergyTotal),
	key(Id::ApparentEnergyA), key(Id::ApparentEnergyB),
	key(Id::ApparentEnergyC), key(Id::ApparentEnergyTotal),
	key(Id::ReactiveEnergyQuadrantIA), key(Id::ReactiveEnergyQuadrantIB),
	key(Id::ReactiveEnergyQuadrantIC), key(Id::ReactiveEnergyQuadrantITotal),
	key(Id::ReactiveEnergyQuadrantIIA), key(Id::ReactiveEnergyQuadrantIIB),
	key(Id::ReactiveEnergyQuadrantIIC), key(Id::ReactiveEnergyQuadrantIITotal),
	key(Id::ReactiveEnergyQuadrantIIIA), key(Id::ReactiveEnergyQuadrantIIIB),
	key(Id::ReactiveEnergyQuadrantIIIC), key(Id::ReactiveEnergyQuadrantIIITotal),
	key(Id::ReactiveEnergyQuadrantIVA), key(Id::ReactiveEnergyQuadrantIVB),
	key(Id::ReactiveEnergyQuadrantIVC), key(Id::ReactiveEnergyQuadrantIVTotal),
	key(Id::CurrentActiveDemandA), key(Id::CurrentActiveDemandB),
	key(Id::CurrentActiveDemandC), key(Id::CurrentActiveDemandTotal),
	key(Id::ImportDemandPeakA), key(Id::ImportDemandPeakB),
	key(Id::ImportDemandPeakC), key(Id::ImportDemandPeakTotal),
	key(Id::ExportDemandPeakA), key(Id::ExportDemandPeakB),
	key(Id::ExportDemandPeakC), key(Id::ExportDemandPeakTotal),
	key(Id::FundamentalVoltageA), key(Id::FundamentalVoltageB),
	key(Id::FundamentalVoltageC), key(Id::FundamentalVoltageLlAB),
	key(Id::FundamentalVoltageLlBC), key(Id::FundamentalVoltageLlCA),
	key(Id::FundamentalCurrentA), key(Id::FundamentalCurrentB),
	key(Id::FundamentalCurrentC), key(Id::FundamentalCurrentN),
	key(Id::VoltageCrestA), key(Id::VoltageCrestB), key(Id::VoltageCrestC),
	key(Id::CurrentCrestA), key(Id::CurrentCrestB), key(Id::CurrentCrestC),
	key(Id::CurrentCrestN), key(Id::FundamentalActivePowerA),
	key(Id::FundamentalActivePowerB), key(Id::FundamentalActivePowerC),
	key(Id::FundamentalActivePowerTotal), key(Id::CurrentPhaseAngleN),
	key(Id::VoltageLlPhaseAngleAB), key(Id::VoltageLlPhaseAngleBC),
	key(Id::VoltageLlPhaseAngleCA), key(Id::DisplacementAngleA),
	key(Id::DisplacementAngleB), key(Id::DisplacementAngleC),
	key(Id::VoltageZeroSequenceAngle), key(Id::VoltagePositiveSequenceAngle),
	key(Id::VoltageNegativeSequenceAngle), key(Id::CurrentZeroSequenceAngle),
	key(Id::CurrentPositiveSequenceAngle), key(Id::CurrentNegativeSequenceAngle),
	key(Id::LoadNatureA), key(Id::LoadNatureB), key(Id::LoadNatureC),
	key(Id::LoadNatureTotal),
};

constexpr std::array labels{
	std::string_view{"Frequency"},
	std::string_view{"Phase A line-to-neutral voltage"},
	std::string_view{"Phase B line-to-neutral voltage"},
	std::string_view{"Phase C line-to-neutral voltage"},
	std::string_view{"Phase AB line-to-line voltage"},
	std::string_view{"Phase BC line-to-line voltage"},
	std::string_view{"Phase CA line-to-line voltage"},
	std::string_view{"Phase A current"},
	std::string_view{"Phase B current"},
	std::string_view{"Phase C current"},
	std::string_view{"Neutral current"},
	std::string_view{"Phase A active power"},
	std::string_view{"Phase B active power"},
	std::string_view{"Phase C active power"},
	std::string_view{"Total active power"},
	std::string_view{"Phase A apparent power"},
	std::string_view{"Phase B apparent power"},
	std::string_view{"Phase C apparent power"},
	std::string_view{"Total apparent power"},
	std::string_view{"Phase A true power factor"},
	std::string_view{"Phase B true power factor"},
	std::string_view{"Phase C true power factor"},
	std::string_view{"Total true power factor"},
	std::string_view{"Phase A fundamental reactive power"},
	std::string_view{"Phase B fundamental reactive power"},
	std::string_view{"Phase C fundamental reactive power"},
	std::string_view{"Total fundamental reactive power"},
	std::string_view{"Phase A displacement power factor"},
	std::string_view{"Phase B displacement power factor"},
	std::string_view{"Phase C displacement power factor"},
	std::string_view{"Total displacement power factor"},
	std::string_view{"Phase A voltage angle"},
	std::string_view{"Phase B voltage angle"},
	std::string_view{"Phase C voltage angle"},
	std::string_view{"Phase A current angle"},
	std::string_view{"Phase B current angle"},
	std::string_view{"Phase C current angle"},
	std::string_view{"Voltage negative-sequence unbalance"},
	std::string_view{"Current negative-sequence unbalance"},
	std::string_view{"Voltage zero-sequence ratio"},
	std::string_view{"Current zero-sequence ratio"},
	std::string_view{"Zero-sequence voltage"},
	std::string_view{"Positive-sequence voltage"},
	std::string_view{"Negative-sequence voltage"},
	std::string_view{"Zero-sequence current"},
	std::string_view{"Positive-sequence current"},
	std::string_view{"Negative-sequence current"},
	std::string_view{"Phase A active import energy"},
	std::string_view{"Phase B active import energy"},
	std::string_view{"Phase C active import energy"},
	std::string_view{"Total active import energy"},
	std::string_view{"Phase A active export energy"},
	std::string_view{"Phase B active export energy"},
	std::string_view{"Phase C active export energy"},
	std::string_view{"Total active export energy"},
	std::string_view{"Phase A apparent energy"},
	std::string_view{"Phase B apparent energy"},
	std::string_view{"Phase C apparent energy"},
	std::string_view{"Total apparent energy"},
	std::string_view{"Phase A quadrant I reactive energy"},
	std::string_view{"Phase B quadrant I reactive energy"},
	std::string_view{"Phase C quadrant I reactive energy"},
	std::string_view{"Total quadrant I reactive energy"},
	std::string_view{"Phase A quadrant II reactive energy"},
	std::string_view{"Phase B quadrant II reactive energy"},
	std::string_view{"Phase C quadrant II reactive energy"},
	std::string_view{"Total quadrant II reactive energy"},
	std::string_view{"Phase A quadrant III reactive energy"},
	std::string_view{"Phase B quadrant III reactive energy"},
	std::string_view{"Phase C quadrant III reactive energy"},
	std::string_view{"Total quadrant III reactive energy"},
	std::string_view{"Phase A quadrant IV reactive energy"},
	std::string_view{"Phase B quadrant IV reactive energy"},
	std::string_view{"Phase C quadrant IV reactive energy"},
	std::string_view{"Total quadrant IV reactive energy"},
	std::string_view{"Phase A current active demand"},
	std::string_view{"Phase B current active demand"},
	std::string_view{"Phase C current active demand"},
	std::string_view{"Total current active demand"},
	std::string_view{"Phase A import demand peak"},
	std::string_view{"Phase B import demand peak"},
	std::string_view{"Phase C import demand peak"},
	std::string_view{"Total import demand peak"},
	std::string_view{"Phase A export demand peak"},
	std::string_view{"Phase B export demand peak"},
	std::string_view{"Phase C export demand peak"},
	std::string_view{"Total export demand peak"},
	std::string_view{"Phase A fundamental voltage"},
	std::string_view{"Phase B fundamental voltage"},
	std::string_view{"Phase C fundamental voltage"},
	std::string_view{"Phase AB fundamental line-to-line voltage"},
	std::string_view{"Phase BC fundamental line-to-line voltage"},
	std::string_view{"Phase CA fundamental line-to-line voltage"},
	std::string_view{"Phase A fundamental current"},
	std::string_view{"Phase B fundamental current"},
	std::string_view{"Phase C fundamental current"},
	std::string_view{"Neutral fundamental current"},
	std::string_view{"Phase A voltage crest factor"},
	std::string_view{"Phase B voltage crest factor"},
	std::string_view{"Phase C voltage crest factor"},
	std::string_view{"Phase A current crest factor"},
	std::string_view{"Phase B current crest factor"},
	std::string_view{"Phase C current crest factor"},
	std::string_view{"Neutral current crest factor"},
	std::string_view{"Phase A fundamental active power"},
	std::string_view{"Phase B fundamental active power"},
	std::string_view{"Phase C fundamental active power"},
	std::string_view{"Total fundamental active power"},
	std::string_view{"Neutral current angle"},
	std::string_view{"Phase AB voltage angle"},
	std::string_view{"Phase BC voltage angle"},
	std::string_view{"Phase CA voltage angle"},
	std::string_view{"Phase A displacement angle"},
	std::string_view{"Phase B displacement angle"},
	std::string_view{"Phase C displacement angle"},
	std::string_view{"Zero-sequence voltage angle"},
	std::string_view{"Positive-sequence voltage angle"},
	std::string_view{"Negative-sequence voltage angle"},
	std::string_view{"Zero-sequence current angle"},
	std::string_view{"Positive-sequence current angle"},
	std::string_view{"Negative-sequence current angle"},
	std::string_view{"Phase A load nature"},
	std::string_view{"Phase B load nature"},
	std::string_view{"Phase C load nature"},
	std::string_view{"Total load nature"},
};

static_assert(labels.size() == catalog.size());

constexpr std::array linear_calculations{
	MeterAttributeCalculation::Minimum,
	MeterAttributeCalculation::Maximum,
	MeterAttributeCalculation::Average,
	MeterAttributeCalculation::Last,
};
constexpr std::array angle_calculations{
	MeterAttributeCalculation::CircularAverage,
	MeterAttributeCalculation::Last,
};
constexpr std::array counter_calculations{
	MeterAttributeCalculation::First,
	MeterAttributeCalculation::Last,
	MeterAttributeCalculation::Delta,
};
constexpr std::array peak_calculations{MeterAttributeCalculation::Last};
constexpr std::array categorical_calculations{MeterAttributeCalculation::Last};

constexpr std::array frequency_aliases{
	std::string_view{"hz"}, std::string_view{"grid frequency"}};
constexpr std::array voltage_aliases{
	std::string_view{"voltage"}, std::string_view{"volts"},
	std::string_view{"rms"}};
constexpr std::array current_aliases{
	std::string_view{"current"}, std::string_view{"amps"},
	std::string_view{"rms"}};
constexpr std::array active_power_aliases{
	std::string_view{"watts"}, std::string_view{"real power"},
	std::string_view{"import export"}};
constexpr std::array apparent_power_aliases{
	std::string_view{"va"}, std::string_view{"apparent power"}};
constexpr std::array power_factor_aliases{
	std::string_view{"pf"}, std::string_view{"power factor"}};
constexpr std::array reactive_power_aliases{
	std::string_view{"var"}, std::string_view{"reactive power"},
	std::string_view{"q1"}};
constexpr std::array angle_aliases{
	std::string_view{"angle"}, std::string_view{"phasor"},
	std::string_view{"degrees"}};
constexpr std::array unbalance_aliases{
	std::string_view{"unbalance"}, std::string_view{"sequence ratio"}};
constexpr std::array sequence_aliases{
	std::string_view{"symmetrical components"},
	std::string_view{"sequence"}};
constexpr std::array energy_aliases{
	std::string_view{"energy"}, std::string_view{"counter"},
	std::string_view{"watt hours"}};
constexpr std::array demand_aliases{
	std::string_view{"demand"}, std::string_view{"peak"},
	std::string_view{"watts"}};
constexpr std::array crest_aliases{
	std::string_view{"crest"}, std::string_view{"peak rms ratio"}};
constexpr std::array fundamental_aliases{
	std::string_view{"fundamental"}, std::string_view{"h1"}};
constexpr std::array load_nature_aliases{
	std::string_view{"load nature"}, std::string_view{"leading lagging"}};

constexpr std::array period_catalog{
	MeasurementPeriodDescriptor{MeasurementPeriod::Basic, "basic",
		"Basic (10/12 cycles)", true, true},
	MeasurementPeriodDescriptor{MeasurementPeriod::Cycles150_180,
		"cycles_150_180", "150/180 cycles", true, true},
	MeasurementPeriodDescriptor{MeasurementPeriod::Min10, "minutes_10",
		"10 minutes", true, true},
	MeasurementPeriodDescriptor{MeasurementPeriod::Hour2, "hours_2",
		"2 hours", true, true},
	MeasurementPeriodDescriptor{MeasurementPeriod::Demand, "demand",
		"Demand", true, true},
	MeasurementPeriodDescriptor{MeasurementPeriod::Min10Live,
		"minutes_10_live", "Open 10-minute preview", true, false},
	MeasurementPeriodDescriptor{MeasurementPeriod::Hour2Live,
		"hours_2_live", "Open 2-hour preview", true, false},
};

constexpr MeterAttributeGroup group_for(Id id)
{
	if (id >= Id::FundamentalVoltageA && id <= Id::FundamentalCurrentN)
		return MeterAttributeGroup::Fundamental;
	if (id >= Id::VoltageCrestA && id <= Id::CurrentCrestN)
		return MeterAttributeGroup::CrestFactor;
	if (id >= Id::FundamentalActivePowerA &&
	    id <= Id::FundamentalActivePowerTotal)
		return MeterAttributeGroup::Fundamental;
	if (id >= Id::CurrentPhaseAngleN &&
	    id <= Id::CurrentNegativeSequenceAngle)
		return MeterAttributeGroup::PhaseAngle;
	if (id >= Id::LoadNatureA)
		return MeterAttributeGroup::LoadNature;
	if (id == Id::Frequency) return MeterAttributeGroup::Frequency;
	if (id <= Id::VcnRms) return MeterAttributeGroup::VoltageLnRms;
	if (id <= Id::VcaRms) return MeterAttributeGroup::VoltageLlRms;
	if (id <= Id::InRms) return MeterAttributeGroup::CurrentRms;
	if (id <= Id::ActivePowerTotal) return MeterAttributeGroup::ActivePower;
	if (id <= Id::ApparentPowerTotal) return MeterAttributeGroup::ApparentPower;
	if (id <= Id::PowerFactorTotal) return MeterAttributeGroup::PowerFactor;
	if (id <= Id::ReactivePowerTotal) return MeterAttributeGroup::ReactivePower;
	if (id <= Id::DisplacementPowerFactorTotal)
		return MeterAttributeGroup::DisplacementPowerFactor;
	if (id <= Id::CurrentPhaseAngleC) return MeterAttributeGroup::PhaseAngle;
	if (id <= Id::CurrentZeroSequenceRatio)
		return MeterAttributeGroup::Unbalance;
	if (id <= Id::NegativeSequenceCurrent)
		return MeterAttributeGroup::SequenceComponents;
	if (id <= Id::ReactiveEnergyQuadrantIVTotal)
		return MeterAttributeGroup::Energy;
	return MeterAttributeGroup::Demand;
}

std::span<const std::string_view> aliases_for(MeterAttributeGroup group)
{
	switch (group) {
	case MeterAttributeGroup::Frequency: return frequency_aliases;
	case MeterAttributeGroup::VoltageLnRms:
	case MeterAttributeGroup::VoltageLlRms: return voltage_aliases;
	case MeterAttributeGroup::CurrentRms: return current_aliases;
	case MeterAttributeGroup::ActivePower: return active_power_aliases;
	case MeterAttributeGroup::ApparentPower: return apparent_power_aliases;
	case MeterAttributeGroup::PowerFactor:
	case MeterAttributeGroup::DisplacementPowerFactor:
		return power_factor_aliases;
	case MeterAttributeGroup::ReactivePower: return reactive_power_aliases;
	case MeterAttributeGroup::PhaseAngle: return angle_aliases;
	case MeterAttributeGroup::Unbalance: return unbalance_aliases;
	case MeterAttributeGroup::SequenceComponents: return sequence_aliases;
	case MeterAttributeGroup::Energy: return energy_aliases;
	case MeterAttributeGroup::Demand: return demand_aliases;
	case MeterAttributeGroup::CrestFactor: return crest_aliases;
	case MeterAttributeGroup::Fundamental: return fundamental_aliases;
	case MeterAttributeGroup::LoadNature: return load_nature_aliases;
	case MeterAttributeGroup::AllDefined: return {};
	}
	return {};
}

MeterAttributeValueKind value_kind_for(Id id)
{
	if ((id >= Id::VoltagePhaseAngleA && id <= Id::CurrentPhaseAngleC) ||
	    (id >= Id::CurrentPhaseAngleN &&
	     id <= Id::CurrentNegativeSequenceAngle))
		return MeterAttributeValueKind::CircularAngle;
	if (id >= Id::ActiveImportEnergyA &&
	    id <= Id::ReactiveEnergyQuadrantIVTotal)
		return MeterAttributeValueKind::CumulativeCounter;
	if (id >= Id::ImportDemandPeakA && id <= Id::ExportDemandPeakTotal)
		return MeterAttributeValueKind::Peak;
	if (id >= Id::LoadNatureA)
		return MeterAttributeValueKind::Categorical;
	return MeterAttributeValueKind::Linear;
}

std::span<const MeterAttributeCalculation> calculations_for(
	MeterAttributeValueKind kind)
{
	switch (kind) {
	case MeterAttributeValueKind::Linear: return linear_calculations;
	case MeterAttributeValueKind::CircularAngle: return angle_calculations;
	case MeterAttributeValueKind::CumulativeCounter:
		return counter_calculations;
	case MeterAttributeValueKind::Peak: return peak_calculations;
	case MeterAttributeValueKind::Categorical:
		return categorical_calculations;
	}
	return {};
}

} // namespace

static MeterAttributeDescriptor describe_identity(MeterAttributeKey attribute)
{
	switch (attribute.id) {
	case Id::Frequency:
		return {attribute, "frequency", MeterUnit::MilliHertz};
	case Id::VanRms:
		return {attribute, "voltage.ln.a.rms", MeterUnit::MicroVolts};
	case Id::VbnRms:
		return {attribute, "voltage.ln.b.rms", MeterUnit::MicroVolts};
	case Id::VcnRms:
		return {attribute, "voltage.ln.c.rms", MeterUnit::MicroVolts};
	case Id::VabRms:
		return {attribute, "voltage.ll.ab.rms", MeterUnit::MicroVolts};
	case Id::VbcRms:
		return {attribute, "voltage.ll.bc.rms", MeterUnit::MicroVolts};
	case Id::VcaRms:
		return {attribute, "voltage.ll.ca.rms", MeterUnit::MicroVolts};
	case Id::IaRms:
		return {attribute, "current.a.rms", MeterUnit::MicroAmperes};
	case Id::IbRms:
		return {attribute, "current.b.rms", MeterUnit::MicroAmperes};
	case Id::IcRms:
		return {attribute, "current.c.rms", MeterUnit::MicroAmperes};
	case Id::InRms:
		return {attribute, "current.n.rms", MeterUnit::MicroAmperes};
	case Id::ActivePowerA:
		return {attribute, "power.active.a", MeterUnit::Picowatts};
	case Id::ActivePowerB:
		return {attribute, "power.active.b", MeterUnit::Picowatts};
	case Id::ActivePowerC:
		return {attribute, "power.active.c", MeterUnit::Picowatts};
	case Id::ActivePowerTotal:
		return {attribute, "power.active.total", MeterUnit::Picowatts};
	case Id::ApparentPowerA:
		return {attribute, "power.apparent.a",
			MeterUnit::PicoVoltAmperes};
	case Id::ApparentPowerB:
		return {attribute, "power.apparent.b",
			MeterUnit::PicoVoltAmperes};
	case Id::ApparentPowerC:
		return {attribute, "power.apparent.c",
			MeterUnit::PicoVoltAmperes};
	case Id::ApparentPowerTotal:
		return {attribute, "power.apparent.total",
			MeterUnit::PicoVoltAmperes};
	case Id::PowerFactorA:
		return {attribute, "power.factor.a",
			MeterUnit::PowerFactorMillionths};
	case Id::PowerFactorB:
		return {attribute, "power.factor.b",
			MeterUnit::PowerFactorMillionths};
	case Id::PowerFactorC:
		return {attribute, "power.factor.c",
			MeterUnit::PowerFactorMillionths};
	case Id::PowerFactorTotal:
		return {attribute, "power.factor.total",
			MeterUnit::PowerFactorMillionths};
	case Id::ReactivePowerA:
		return {attribute, "power.reactive.a", MeterUnit::Picovars};
	case Id::ReactivePowerB:
		return {attribute, "power.reactive.b", MeterUnit::Picovars};
	case Id::ReactivePowerC:
		return {attribute, "power.reactive.c", MeterUnit::Picovars};
	case Id::ReactivePowerTotal:
		return {attribute, "power.reactive.total", MeterUnit::Picovars};
	case Id::DisplacementPowerFactorA:
		return {attribute, "power.factor.displacement.a",
			MeterUnit::PowerFactorMillionths};
	case Id::DisplacementPowerFactorB:
		return {attribute, "power.factor.displacement.b",
			MeterUnit::PowerFactorMillionths};
	case Id::DisplacementPowerFactorC:
		return {attribute, "power.factor.displacement.c",
			MeterUnit::PowerFactorMillionths};
	case Id::DisplacementPowerFactorTotal:
		return {attribute, "power.factor.displacement.total",
			MeterUnit::PowerFactorMillionths};
	case Id::VoltagePhaseAngleA:
		return {attribute, "phase.angle.voltage.a",
			MeterUnit::Millidegrees};
	case Id::VoltagePhaseAngleB:
		return {attribute, "phase.angle.voltage.b",
			MeterUnit::Millidegrees};
	case Id::VoltagePhaseAngleC:
		return {attribute, "phase.angle.voltage.c",
			MeterUnit::Millidegrees};
	case Id::CurrentPhaseAngleA:
		return {attribute, "phase.angle.current.a",
			MeterUnit::Millidegrees};
	case Id::CurrentPhaseAngleB:
		return {attribute, "phase.angle.current.b",
			MeterUnit::Millidegrees};
	case Id::CurrentPhaseAngleC:
		return {attribute, "phase.angle.current.c",
			MeterUnit::Millidegrees};
	case Id::VoltageUnbalance:
		return {attribute, "unbalance.voltage",
			MeterUnit::RatioMillionths};
	case Id::CurrentUnbalance:
		return {attribute, "unbalance.current",
			MeterUnit::RatioMillionths};
	case Id::VoltageZeroSequenceRatio:
		return {attribute, "unbalance.voltage.zero",
			MeterUnit::RatioMillionths};
	case Id::CurrentZeroSequenceRatio:
		return {attribute, "unbalance.current.zero",
			MeterUnit::RatioMillionths};
	case Id::ZeroSequenceVoltage:
		return {attribute, "sequence.voltage.zero.rms",
			MeterUnit::MicroVolts};
	case Id::PositiveSequenceVoltage:
		return {attribute, "sequence.voltage.positive.rms",
			MeterUnit::MicroVolts};
	case Id::NegativeSequenceVoltage:
		return {attribute, "sequence.voltage.negative.rms",
			MeterUnit::MicroVolts};
	case Id::ZeroSequenceCurrent:
		return {attribute, "sequence.current.zero.rms",
			MeterUnit::MicroAmperes};
	case Id::PositiveSequenceCurrent:
		return {attribute, "sequence.current.positive.rms",
			MeterUnit::MicroAmperes};
	case Id::NegativeSequenceCurrent:
		return {attribute, "sequence.current.negative.rms",
			MeterUnit::MicroAmperes};
	case Id::ActiveImportEnergyA:
		return {attribute, "energy.active.import.a", MeterUnit::MicroWattHours};
	case Id::ActiveImportEnergyB:
		return {attribute, "energy.active.import.b", MeterUnit::MicroWattHours};
	case Id::ActiveImportEnergyC:
		return {attribute, "energy.active.import.c", MeterUnit::MicroWattHours};
	case Id::ActiveImportEnergyTotal:
		return {attribute, "energy.active.import.total", MeterUnit::MicroWattHours};
	case Id::ActiveExportEnergyA:
		return {attribute, "energy.active.export.a", MeterUnit::MicroWattHours};
	case Id::ActiveExportEnergyB:
		return {attribute, "energy.active.export.b", MeterUnit::MicroWattHours};
	case Id::ActiveExportEnergyC:
		return {attribute, "energy.active.export.c", MeterUnit::MicroWattHours};
	case Id::ActiveExportEnergyTotal:
		return {attribute, "energy.active.export.total", MeterUnit::MicroWattHours};
	case Id::ApparentEnergyA:
		return {attribute, "energy.apparent.a", MeterUnit::MicroVoltAmpereHours};
	case Id::ApparentEnergyB:
		return {attribute, "energy.apparent.b", MeterUnit::MicroVoltAmpereHours};
	case Id::ApparentEnergyC:
		return {attribute, "energy.apparent.c", MeterUnit::MicroVoltAmpereHours};
	case Id::ApparentEnergyTotal:
		return {attribute, "energy.apparent.total", MeterUnit::MicroVoltAmpereHours};
	case Id::ReactiveEnergyQuadrantIA:
		return {attribute, "energy.reactive.quadrant_i.a", MeterUnit::MicroVarHours};
	case Id::ReactiveEnergyQuadrantIB:
		return {attribute, "energy.reactive.quadrant_i.b", MeterUnit::MicroVarHours};
	case Id::ReactiveEnergyQuadrantIC:
		return {attribute, "energy.reactive.quadrant_i.c", MeterUnit::MicroVarHours};
	case Id::ReactiveEnergyQuadrantITotal:
		return {attribute, "energy.reactive.quadrant_i.total", MeterUnit::MicroVarHours};
	case Id::ReactiveEnergyQuadrantIIA:
		return {attribute, "energy.reactive.quadrant_ii.a", MeterUnit::MicroVarHours};
	case Id::ReactiveEnergyQuadrantIIB:
		return {attribute, "energy.reactive.quadrant_ii.b", MeterUnit::MicroVarHours};
	case Id::ReactiveEnergyQuadrantIIC:
		return {attribute, "energy.reactive.quadrant_ii.c", MeterUnit::MicroVarHours};
	case Id::ReactiveEnergyQuadrantIITotal:
		return {attribute, "energy.reactive.quadrant_ii.total", MeterUnit::MicroVarHours};
	case Id::ReactiveEnergyQuadrantIIIA:
		return {attribute, "energy.reactive.quadrant_iii.a", MeterUnit::MicroVarHours};
	case Id::ReactiveEnergyQuadrantIIIB:
		return {attribute, "energy.reactive.quadrant_iii.b", MeterUnit::MicroVarHours};
	case Id::ReactiveEnergyQuadrantIIIC:
		return {attribute, "energy.reactive.quadrant_iii.c", MeterUnit::MicroVarHours};
	case Id::ReactiveEnergyQuadrantIIITotal:
		return {attribute, "energy.reactive.quadrant_iii.total", MeterUnit::MicroVarHours};
	case Id::ReactiveEnergyQuadrantIVA:
		return {attribute, "energy.reactive.quadrant_iv.a", MeterUnit::MicroVarHours};
	case Id::ReactiveEnergyQuadrantIVB:
		return {attribute, "energy.reactive.quadrant_iv.b", MeterUnit::MicroVarHours};
	case Id::ReactiveEnergyQuadrantIVC:
		return {attribute, "energy.reactive.quadrant_iv.c", MeterUnit::MicroVarHours};
	case Id::ReactiveEnergyQuadrantIVTotal:
		return {attribute, "energy.reactive.quadrant_iv.total", MeterUnit::MicroVarHours};
	case Id::CurrentActiveDemandA:
		return {attribute, "demand.active.current.a", MeterUnit::MicroWatts};
	case Id::CurrentActiveDemandB:
		return {attribute, "demand.active.current.b", MeterUnit::MicroWatts};
	case Id::CurrentActiveDemandC:
		return {attribute, "demand.active.current.c", MeterUnit::MicroWatts};
	case Id::CurrentActiveDemandTotal:
		return {attribute, "demand.active.current.total", MeterUnit::MicroWatts};
	case Id::ImportDemandPeakA:
		return {attribute, "demand.active.import_peak.a", MeterUnit::MicroWatts};
	case Id::ImportDemandPeakB:
		return {attribute, "demand.active.import_peak.b", MeterUnit::MicroWatts};
	case Id::ImportDemandPeakC:
		return {attribute, "demand.active.import_peak.c", MeterUnit::MicroWatts};
	case Id::ImportDemandPeakTotal:
		return {attribute, "demand.active.import_peak.total", MeterUnit::MicroWatts};
	case Id::ExportDemandPeakA:
		return {attribute, "demand.active.export_peak.a", MeterUnit::MicroWatts};
	case Id::ExportDemandPeakB:
		return {attribute, "demand.active.export_peak.b", MeterUnit::MicroWatts};
	case Id::ExportDemandPeakC:
		return {attribute, "demand.active.export_peak.c", MeterUnit::MicroWatts};
	case Id::ExportDemandPeakTotal:
		return {attribute, "demand.active.export_peak.total", MeterUnit::MicroWatts};
	case Id::FundamentalVoltageA:
		return {attribute, "voltage.fundamental.a.rms", MeterUnit::MicroVolts};
	case Id::FundamentalVoltageB:
		return {attribute, "voltage.fundamental.b.rms", MeterUnit::MicroVolts};
	case Id::FundamentalVoltageC:
		return {attribute, "voltage.fundamental.c.rms", MeterUnit::MicroVolts};
	case Id::FundamentalVoltageLlAB:
		return {attribute, "voltage.fundamental.ll.ab.rms", MeterUnit::MicroVolts};
	case Id::FundamentalVoltageLlBC:
		return {attribute, "voltage.fundamental.ll.bc.rms", MeterUnit::MicroVolts};
	case Id::FundamentalVoltageLlCA:
		return {attribute, "voltage.fundamental.ll.ca.rms", MeterUnit::MicroVolts};
	case Id::FundamentalCurrentA:
		return {attribute, "current.fundamental.a.rms", MeterUnit::MicroAmperes};
	case Id::FundamentalCurrentB:
		return {attribute, "current.fundamental.b.rms", MeterUnit::MicroAmperes};
	case Id::FundamentalCurrentC:
		return {attribute, "current.fundamental.c.rms", MeterUnit::MicroAmperes};
	case Id::FundamentalCurrentN:
		return {attribute, "current.fundamental.n.rms", MeterUnit::MicroAmperes};
	case Id::VoltageCrestA:
		return {attribute, "crest.voltage.a", MeterUnit::CrestTenThousandths};
	case Id::VoltageCrestB:
		return {attribute, "crest.voltage.b", MeterUnit::CrestTenThousandths};
	case Id::VoltageCrestC:
		return {attribute, "crest.voltage.c", MeterUnit::CrestTenThousandths};
	case Id::CurrentCrestA:
		return {attribute, "crest.current.a", MeterUnit::CrestTenThousandths};
	case Id::CurrentCrestB:
		return {attribute, "crest.current.b", MeterUnit::CrestTenThousandths};
	case Id::CurrentCrestC:
		return {attribute, "crest.current.c", MeterUnit::CrestTenThousandths};
	case Id::CurrentCrestN:
		return {attribute, "crest.current.n", MeterUnit::CrestTenThousandths};
	case Id::FundamentalActivePowerA:
		return {attribute, "power.active.fundamental.a", MeterUnit::Picowatts};
	case Id::FundamentalActivePowerB:
		return {attribute, "power.active.fundamental.b", MeterUnit::Picowatts};
	case Id::FundamentalActivePowerC:
		return {attribute, "power.active.fundamental.c", MeterUnit::Picowatts};
	case Id::FundamentalActivePowerTotal:
		return {attribute, "power.active.fundamental.total", MeterUnit::Picowatts};
	case Id::CurrentPhaseAngleN:
		return {attribute, "phase.angle.current.n", MeterUnit::Millidegrees};
	case Id::VoltageLlPhaseAngleAB:
		return {attribute, "phase.angle.voltage.ll.ab", MeterUnit::Millidegrees};
	case Id::VoltageLlPhaseAngleBC:
		return {attribute, "phase.angle.voltage.ll.bc", MeterUnit::Millidegrees};
	case Id::VoltageLlPhaseAngleCA:
		return {attribute, "phase.angle.voltage.ll.ca", MeterUnit::Millidegrees};
	case Id::DisplacementAngleA:
		return {attribute, "phase.angle.displacement.a", MeterUnit::Millidegrees};
	case Id::DisplacementAngleB:
		return {attribute, "phase.angle.displacement.b", MeterUnit::Millidegrees};
	case Id::DisplacementAngleC:
		return {attribute, "phase.angle.displacement.c", MeterUnit::Millidegrees};
	case Id::VoltageZeroSequenceAngle:
		return {attribute, "sequence.voltage.zero.angle", MeterUnit::Millidegrees};
	case Id::VoltagePositiveSequenceAngle:
		return {attribute, "sequence.voltage.positive.angle", MeterUnit::Millidegrees};
	case Id::VoltageNegativeSequenceAngle:
		return {attribute, "sequence.voltage.negative.angle", MeterUnit::Millidegrees};
	case Id::CurrentZeroSequenceAngle:
		return {attribute, "sequence.current.zero.angle", MeterUnit::Millidegrees};
	case Id::CurrentPositiveSequenceAngle:
		return {attribute, "sequence.current.positive.angle", MeterUnit::Millidegrees};
	case Id::CurrentNegativeSequenceAngle:
		return {attribute, "sequence.current.negative.angle", MeterUnit::Millidegrees};
	case Id::LoadNatureA:
		return {attribute, "load.nature.a", MeterUnit::CategoricalCode};
	case Id::LoadNatureB:
		return {attribute, "load.nature.b", MeterUnit::CategoricalCode};
	case Id::LoadNatureC:
		return {attribute, "load.nature.c", MeterUnit::CategoricalCode};
	case Id::LoadNatureTotal:
		return {attribute, "load.nature.total", MeterUnit::CategoricalCode};
	}
	throw std::invalid_argument("unknown meter attribute");
}

MeterAttributeDescriptor describe(MeterAttributeKey attribute)
{
	auto result = describe_identity(attribute);
	const auto index = static_cast<std::size_t>(attribute.id);
	if (index >= catalog.size() || catalog[index].id != attribute.id)
		throw std::invalid_argument("unknown meter attribute");
	result.label = labels[index];
	result.group = group_for(attribute.id);
	result.value_kind = value_kind_for(attribute.id);
	result.calculations = calculations_for(result.value_kind);
	result.search_aliases = aliases_for(result.group);
	return result;
}

std::optional<MeterAttributeKey> find_attribute(std::string_view name) noexcept
{
	for (const auto attribute : catalog) {
		try {
			if (describe(attribute).key == name)
				return attribute;
		} catch (...) {
		}
	}
	return std::nullopt;
}

std::span<const MeterAttributeKey> defined_attributes() noexcept
{
	return catalog;
}

std::span<const MeasurementPeriodDescriptor>
defined_measurement_periods() noexcept
{
	return period_catalog;
}

bool supports_attribute(MeterAttributeKey attribute,
	MeasurementPeriod period, MeterAttributeUsage usage) noexcept
{
	if (attribute.index)
		return false;
	const auto numeric = static_cast<std::size_t>(attribute.id);
	if (numeric >= catalog.size() || catalog[numeric].id != attribute.id)
		return false;

	const auto id = attribute.id;
	const bool energy = id >= Id::ActiveImportEnergyA &&
		id <= Id::ReactiveEnergyQuadrantIVTotal;
	const bool demand = id >= Id::CurrentActiveDemandA &&
		id <= Id::ExportDemandPeakTotal;
	if (demand)
		return period == MeasurementPeriod::Demand;
	if (energy) {
		return usage == MeterAttributeUsage::Snapshot
			? period == MeasurementPeriod::Basic
			: period == MeasurementPeriod::Min10;
	}
	if (id == Id::Frequency)
		return period == MeasurementPeriod::Basic;
	if (usage == MeterAttributeUsage::Historian &&
	    (period == MeasurementPeriod::Min10Live ||
	     period == MeasurementPeriod::Hour2Live))
		return false;
	return period == MeasurementPeriod::Basic ||
		period == MeasurementPeriod::Cycles150_180 ||
		period == MeasurementPeriod::Min10 ||
		period == MeasurementPeriod::Hour2 ||
		period == MeasurementPeriod::Min10Live ||
		period == MeasurementPeriod::Hour2Live;
}

std::vector<MeterAttributeKey> attributes_for(MeasurementPeriod period,
	MeterAttributeUsage usage)
{
	std::vector<MeterAttributeKey> result;
	result.reserve(catalog.size());
	for (const auto attribute : catalog)
		if (supports_attribute(attribute, period, usage))
			result.push_back(attribute);
	return result;
}

std::string_view unit_name(MeterUnit unit) noexcept
{
	switch (unit) {
	case MeterUnit::MilliHertz: return "mHz";
	case MeterUnit::MicroVolts: return "uV";
	case MeterUnit::MicroAmperes: return "uA";
	case MeterUnit::Picowatts: return "pW";
	case MeterUnit::PicoVoltAmperes: return "pVA";
	case MeterUnit::PowerFactorMillionths: return "pf_e6";
	case MeterUnit::Picovars: return "pvar";
	case MeterUnit::Millidegrees: return "mdeg";
	case MeterUnit::RatioMillionths: return "ratio_e6";
	case MeterUnit::MicroWattHours: return "uWh";
	case MeterUnit::MicroVarHours: return "uvarh";
	case MeterUnit::MicroVoltAmpereHours: return "uVAh";
	case MeterUnit::MicroWatts: return "uW";
	case MeterUnit::CrestTenThousandths: return "crest_e4";
	case MeterUnit::CategoricalCode: return "code";
	}
	return "unknown";
}

std::vector<MeterAttributeKey> attributes_in(MeterAttributeGroup group)
{
	switch (group) {
	case MeterAttributeGroup::Frequency:
		return {key(Id::Frequency)};
	case MeterAttributeGroup::VoltageLnRms:
		return {key(Id::VanRms), key(Id::VbnRms), key(Id::VcnRms)};
	case MeterAttributeGroup::VoltageLlRms:
		return {key(Id::VabRms), key(Id::VbcRms), key(Id::VcaRms)};
	case MeterAttributeGroup::CurrentRms:
		return {key(Id::IaRms), key(Id::IbRms), key(Id::IcRms),
			key(Id::InRms)};
	case MeterAttributeGroup::ActivePower:
		return {key(Id::ActivePowerA), key(Id::ActivePowerB),
			key(Id::ActivePowerC), key(Id::ActivePowerTotal)};
	case MeterAttributeGroup::ApparentPower:
		return {key(Id::ApparentPowerA), key(Id::ApparentPowerB),
			key(Id::ApparentPowerC), key(Id::ApparentPowerTotal)};
	case MeterAttributeGroup::PowerFactor:
		return {key(Id::PowerFactorA), key(Id::PowerFactorB),
			key(Id::PowerFactorC), key(Id::PowerFactorTotal)};
	case MeterAttributeGroup::ReactivePower:
		return {key(Id::ReactivePowerA), key(Id::ReactivePowerB),
			key(Id::ReactivePowerC), key(Id::ReactivePowerTotal)};
	case MeterAttributeGroup::DisplacementPowerFactor:
		return {key(Id::DisplacementPowerFactorA),
			key(Id::DisplacementPowerFactorB),
			key(Id::DisplacementPowerFactorC),
			key(Id::DisplacementPowerFactorTotal)};
	case MeterAttributeGroup::PhaseAngle:
		return {key(Id::VoltagePhaseAngleA), key(Id::VoltagePhaseAngleB),
			key(Id::VoltagePhaseAngleC), key(Id::CurrentPhaseAngleA),
			key(Id::CurrentPhaseAngleB), key(Id::CurrentPhaseAngleC),
			key(Id::CurrentPhaseAngleN), key(Id::VoltageLlPhaseAngleAB),
			key(Id::VoltageLlPhaseAngleBC), key(Id::VoltageLlPhaseAngleCA),
			key(Id::DisplacementAngleA), key(Id::DisplacementAngleB),
			key(Id::DisplacementAngleC), key(Id::VoltageZeroSequenceAngle),
			key(Id::VoltagePositiveSequenceAngle),
			key(Id::VoltageNegativeSequenceAngle),
			key(Id::CurrentZeroSequenceAngle),
			key(Id::CurrentPositiveSequenceAngle),
			key(Id::CurrentNegativeSequenceAngle)};
	case MeterAttributeGroup::Unbalance:
		return {key(Id::VoltageUnbalance), key(Id::CurrentUnbalance),
			key(Id::VoltageZeroSequenceRatio),
			key(Id::CurrentZeroSequenceRatio)};
	case MeterAttributeGroup::SequenceComponents:
		return {key(Id::ZeroSequenceVoltage),
			key(Id::PositiveSequenceVoltage),
			key(Id::NegativeSequenceVoltage),
			key(Id::ZeroSequenceCurrent),
			key(Id::PositiveSequenceCurrent),
			key(Id::NegativeSequenceCurrent)};
	case MeterAttributeGroup::Energy:
		return {key(Id::ActiveImportEnergyA),
			key(Id::ActiveImportEnergyB), key(Id::ActiveImportEnergyC),
			key(Id::ActiveImportEnergyTotal), key(Id::ActiveExportEnergyA),
			key(Id::ActiveExportEnergyB), key(Id::ActiveExportEnergyC),
			key(Id::ActiveExportEnergyTotal), key(Id::ApparentEnergyA),
			key(Id::ApparentEnergyB), key(Id::ApparentEnergyC),
			key(Id::ApparentEnergyTotal),
			key(Id::ReactiveEnergyQuadrantIA),
			key(Id::ReactiveEnergyQuadrantIB),
			key(Id::ReactiveEnergyQuadrantIC),
			key(Id::ReactiveEnergyQuadrantITotal),
			key(Id::ReactiveEnergyQuadrantIIA),
			key(Id::ReactiveEnergyQuadrantIIB),
			key(Id::ReactiveEnergyQuadrantIIC),
			key(Id::ReactiveEnergyQuadrantIITotal),
			key(Id::ReactiveEnergyQuadrantIIIA),
			key(Id::ReactiveEnergyQuadrantIIIB),
			key(Id::ReactiveEnergyQuadrantIIIC),
			key(Id::ReactiveEnergyQuadrantIIITotal),
			key(Id::ReactiveEnergyQuadrantIVA),
			key(Id::ReactiveEnergyQuadrantIVB),
			key(Id::ReactiveEnergyQuadrantIVC),
			key(Id::ReactiveEnergyQuadrantIVTotal)};
	case MeterAttributeGroup::Demand:
		return {key(Id::CurrentActiveDemandA),
			key(Id::CurrentActiveDemandB), key(Id::CurrentActiveDemandC),
			key(Id::CurrentActiveDemandTotal), key(Id::ImportDemandPeakA),
			key(Id::ImportDemandPeakB), key(Id::ImportDemandPeakC),
			key(Id::ImportDemandPeakTotal), key(Id::ExportDemandPeakA),
			key(Id::ExportDemandPeakB), key(Id::ExportDemandPeakC),
			key(Id::ExportDemandPeakTotal)};
	case MeterAttributeGroup::Fundamental:
		return {key(Id::Frequency), key(Id::VanRms), key(Id::VbnRms),
			key(Id::VcnRms), key(Id::VabRms), key(Id::VbcRms),
			key(Id::VcaRms), key(Id::IaRms), key(Id::IbRms),
			key(Id::IcRms), key(Id::InRms),
			key(Id::FundamentalVoltageA), key(Id::FundamentalVoltageB),
			key(Id::FundamentalVoltageC), key(Id::FundamentalVoltageLlAB),
			key(Id::FundamentalVoltageLlBC),
			key(Id::FundamentalVoltageLlCA),
			key(Id::FundamentalCurrentA), key(Id::FundamentalCurrentB),
			key(Id::FundamentalCurrentC), key(Id::FundamentalCurrentN),
			key(Id::FundamentalActivePowerA),
			key(Id::FundamentalActivePowerB),
			key(Id::FundamentalActivePowerC),
			key(Id::FundamentalActivePowerTotal)};
	case MeterAttributeGroup::CrestFactor:
		return {key(Id::VoltageCrestA), key(Id::VoltageCrestB),
			key(Id::VoltageCrestC), key(Id::CurrentCrestA),
			key(Id::CurrentCrestB), key(Id::CurrentCrestC),
			key(Id::CurrentCrestN)};
	case MeterAttributeGroup::LoadNature:
		return {key(Id::LoadNatureA), key(Id::LoadNatureB),
			key(Id::LoadNatureC), key(Id::LoadNatureTotal)};
	case MeterAttributeGroup::AllDefined:
		return std::vector<MeterAttributeKey>(catalog.begin(), catalog.end());
	}
	return {};
}

MeterAttributeSet::MeterAttributeSet(std::vector<MeterAttributeKey> attributes)
{
	for (const auto attribute : attributes)
		add(attribute);
}

void MeterAttributeSet::add(MeterAttributeKey attribute)
{
	if (!contains(attribute))
		attributes_.push_back(attribute);
}

void MeterAttributeSet::add(MeterAttributeGroup group)
{
	for (const auto attribute : attributes_in(group))
		add(attribute);
}

bool MeterAttributeSet::contains(MeterAttributeKey attribute) const
{
	return std::ranges::find(attributes_, attribute) != attributes_.end();
}

} // namespace mnc::meter
