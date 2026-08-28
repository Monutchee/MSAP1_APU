#pragma once

#include "msap1/meter/meter_data.hpp"
#include "msap1/meter/meter_record.hpp"

#include <cstdint>
#include <array>
#include <optional>

namespace msap1 {

inline constexpr std::size_t energy_counter_count = 28;
using EnergyCounterArray = std::array<std::int64_t, energy_counter_count>;
using DemandValueArray = std::array<std::int64_t, 4>;

[[nodiscard]] EnergyCounterArray flatten_energy_counters(
	const EnergyValues &values) noexcept;
void assign_energy_counters(EnergyValues &values,
	const EnergyCounterArray &counters) noexcept;
[[nodiscard]] DemandValueArray flatten_demand_values(
	const PhaseABCTotal<Reading<MicroWatts>> &values) noexcept;
void assign_demand_values(PhaseABCTotal<Reading<MicroWatts>> &values,
	const DemandValueArray &source) noexcept;

struct EnergyFamilyIdentity {
	std::uint32_t sequence = 0;
	std::uint32_t configuration_generation = 0;
	std::uint32_t sample_rate_hz = 0;
	std::uint32_t sample_count = 0;
	std::uint8_t source_valid_mask = 0;
	std::uint32_t status = 0;
	std::uint64_t first_sample_index = 0;
	std::uint64_t last_sample_index = 0;
	std::uint64_t session_id = 0;
	std::uint64_t accepted_samples = 0;
	std::uint64_t skipped_samples = 0;
	std::uint32_t accepted_blocks = 0;
	std::uint32_t skipped_blocks = 0;

	bool operator==(const EnergyFamilyIdentity &) const = default;
};

struct EnergyAssemblyUpdate {
	std::optional<EnergyValues> completed{};
	std::uint64_t incomplete_families = 0;
	bool duplicate_part = false;
};

/** Bounded atomic assembler for the two ENERGY-v1 records. */
class EnergyFamilyAssembler final {
public:
	[[nodiscard]] EnergyAssemblyUpdate accept(const MeterRecord &record,
		SystemTime received_at = std::chrono::system_clock::now());
	void reset() noexcept;
	[[nodiscard]] std::size_t pending_parts() const noexcept;

private:
	std::optional<EnergyFamilyIdentity> identity_{};
	std::optional<MeterRecord> summary_{};
	std::optional<MeterRecord> quadrants_{};
};

[[nodiscard]] EnergyFamilyIdentity decode_energy_identity(
	const MeterRecord &record);
[[nodiscard]] EnergyValues decode_energy_family(
	const MeterRecord &summary, const MeterRecord &quadrants,
	SystemTime received_at = std::chrono::system_clock::now());
[[nodiscard]] MeterUpdate decode_demand_meter_record(
	const MeterRecord &record,
	SystemTime received_at = std::chrono::system_clock::now());

} // namespace msap1
