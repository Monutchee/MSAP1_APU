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
	/** Reserved for a future PL ten-minute product. */
	Min10,
	/** Reserved for a future PL two-hour product. */
	Hour2,
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
	AllDefined,
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
