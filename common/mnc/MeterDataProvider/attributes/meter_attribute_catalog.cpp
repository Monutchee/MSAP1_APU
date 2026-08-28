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
};

} // namespace

MeterAttributeDescriptor describe(MeterAttributeKey attribute)
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
	}
	throw std::invalid_argument("unknown meter attribute");
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
			key(Id::CurrentPhaseAngleB), key(Id::CurrentPhaseAngleC)};
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
			key(Id::IcRms), key(Id::InRms)};
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
