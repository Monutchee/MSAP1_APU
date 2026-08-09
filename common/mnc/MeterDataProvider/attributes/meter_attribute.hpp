#pragma once

#include <cstdint>
#include <compare>
#include <optional>
#include <string>

namespace mnc::meter {

enum class MeasurementPeriod : std::uint8_t {
	/** Current PL fundamental block (nominally 200 ms today). */
	Basic = 0,
	/** IEC-style 150/180-cycle aggregate supplied by the PL. */
	Cycles150_180,
	/** Reserved for a future PL ten-minute product. */
	Min10,
	/** Reserved for a future PL two-hour product. */
	Hour2,
};

enum class MeterAttributeId : std::uint16_t {
	Frequency = 0,
	VanRms,
	VbnRms,
	VcnRms,
	VabRms,
	VbcRms,
	VcaRms,
	IaRms,
	IbRms,
	IcRms,
	InRms,
};

struct MeterAttributeKey {
	MeterAttributeId id = MeterAttributeId::Frequency;
	std::optional<std::uint16_t> index;

	auto operator<=>(const MeterAttributeKey &) const = default;
};

enum class MeterAttributeGroup : std::uint8_t {
	Frequency,
	VoltageLnRms,
	VoltageLlRms,
	CurrentRms,
	Fundamental,
	AllAvailable,
};

enum class MeterUnit : std::uint8_t {
	MilliHertz,
	MicroVolts,
	MicroAmperes,
};

enum class ReadingQuality : std::uint8_t {
	Unavailable = 0,
	Valid,
	Invalid,
	OutOfRange,
	TimedOut,
	ArithmeticError,
};

struct MeterAttributeDescriptor {
	MeterAttributeKey attribute;
	std::string key;
	MeterUnit unit = MeterUnit::MicroVolts;
};

[[nodiscard]] MeterAttributeDescriptor describe(MeterAttributeKey attribute);

} // namespace mnc::meter
