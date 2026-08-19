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
	case MeterAttributeGroup::Fundamental:
	case MeterAttributeGroup::AllDefined:
		return {key(Id::Frequency), key(Id::VanRms), key(Id::VbnRms),
			key(Id::VcnRms), key(Id::VabRms), key(Id::VbcRms),
			key(Id::VcaRms), key(Id::IaRms), key(Id::IbRms),
			key(Id::IcRms), key(Id::InRms)};
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
