#pragma once

#include <cstdint>
#include <compare>
#include <optional>
#include <span>
#include <string_view>

namespace mnc::meter {

enum class MeasurementPeriod : std::uint8_t {
	/**
	 * Fundamental cycle-defined measurement block:
	 * 10 complete cycles at 50 Hz nominal,
	 * 12 complete cycles at 60 Hz nominal.
	 */
	Basic = 0,
	/**
	 * Aggregate of 15 consecutive eligible Basic blocks:
	 * 150 cycles at 50 Hz nominal,
	 * 180 cycles at 60 Hz nominal.
	 */
	Cycles150_180,
	/** Clock-aligned ten-minute aggregate of eligible Basic blocks. */
	Min10,
	/** Twelve completed, aligned ten-minute intervals. */
	Hour2,
	/** Non-normative view of the currently open ten-minute interval. */
	Min10Live,
	/** Non-normative view of the currently open two-hour interval. */
	Hour2Live,
};

/**
 * Stable protocol-independent logical measurement identity.
 *
 * Protocol adapters map these IDs to their own addressing, such as Web JSON
 * keys, MQTT topics, or Modbus registers.  The enum value is not itself a
 * wire address, topic, route, or register number.
 */
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
	/* M8, the 10/12-cycle power tier. Appended: attribute ids are a
	 * stable external contract and must never be renumbered. */
	ActivePowerA,
	ActivePowerB,
	ActivePowerC,
	ActivePowerTotal,
	ApparentPowerA,
	ApparentPowerB,
	ApparentPowerC,
	ApparentPowerTotal,
	PowerFactorA,
	PowerFactorB,
	PowerFactorC,
	PowerFactorTotal,
	/* M9, the 10/12-cycle phasor tier (fundamental quantities from the
	 * synchronous correlation). Appended — never renumber. */
	ReactivePowerA,
	ReactivePowerB,
	ReactivePowerC,
	ReactivePowerTotal,
	DisplacementPowerFactorA,
	DisplacementPowerFactorB,
	DisplacementPowerFactorC,
	DisplacementPowerFactorTotal,
	/* Fundamental phase angles, millidegrees relative to Va (Va = 0). */
	VoltagePhaseAngleA,
	VoltagePhaseAngleB,
	VoltagePhaseAngleC,
	CurrentPhaseAngleA,
	CurrentPhaseAngleB,
	CurrentPhaseAngleC,
	/* M10, symmetrical components + unbalance. Appended — never
	 * renumber. Ratios are millionths of the positive sequence. */
	VoltageUnbalance,
	CurrentUnbalance,
	VoltageZeroSequenceRatio,
	CurrentZeroSequenceRatio,
	ZeroSequenceVoltage,
	PositiveSequenceVoltage,
	NegativeSequenceVoltage,
	ZeroSequenceCurrent,
	PositiveSequenceCurrent,
	NegativeSequenceCurrent,
};

/**
 * Generic logical attribute key.
 *
 * `id` is the canonical C++ identity.  `index` is reserved for indexed
 * measurement families such as harmonic order; scalar measurements must use
 * std::nullopt.  The descriptor's textual key is the stable external name,
 * while each protocol adapter remains responsible for its own addressing.
 */
struct MeterAttributeKey {
	MeterAttributeId id = MeterAttributeId::Frequency;
	std::optional<std::uint16_t> index;

	auto operator<=>(const MeterAttributeKey &) const = default;
};

/**
 * Convenience families from the generic attribute catalog.
 *
 * Group membership describes logical relationships, not measurements a
 * particular provider currently supplies.  Consult provider capabilities for
 * runtime availability.
 */
enum class MeterAttributeGroup : std::uint8_t {
	Frequency,
	VoltageLnRms,
	VoltageLlRms,
	CurrentRms,
	Fundamental,
	ActivePower,
	ApparentPower,
	PowerFactor,
	ReactivePower,
	DisplacementPowerFactor,
	PhaseAngle,
	Unbalance,
	SequenceComponents,
	AllDefined,
};

enum class MeterUnit : std::uint8_t {
	MilliHertz,
	MicroVolts,
	MicroAmperes,
	Picowatts,
	PicoVoltAmperes,
	/* True power factor, signed millionths; undefined (Unavailable
	 * quality) when the apparent power is zero. */
	PowerFactorMillionths,
	/* Fundamental reactive power Q1, signed picovars; lagging positive. */
	Picovars,
	/* Phase angle, millidegrees in [0, 360000) (the PL publishes the
	 * industry convention directly), relative to the Va fundamental. */
	Millidegrees,
	/* Unsigned ratio in millionths of the positive-sequence magnitude
	 * (20000 = 2%); undefined (Unavailable) when that magnitude is 0. */
	RatioMillionths,
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
	/** Stable external textual identity; backed by static catalog storage. */
	std::string_view key;
	MeterUnit unit = MeterUnit::MicroVolts;
};

[[nodiscard]] MeterAttributeDescriptor describe(MeterAttributeKey attribute);

/** Resolve a stable external catalog key to its logical attribute. */
[[nodiscard]] std::optional<MeterAttributeKey>
find_attribute(std::string_view key) noexcept;

/** Enumerate the stable scalar catalog in declaration order. */
[[nodiscard]] std::span<const MeterAttributeKey> defined_attributes() noexcept;

} // namespace mnc::meter
