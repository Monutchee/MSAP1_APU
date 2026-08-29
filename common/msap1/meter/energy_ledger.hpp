#pragma once

#include "msap1/meter/energy_demand.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace msap1::energy_ledger {

class Unavailable : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

class Conflict : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

struct ResetRequest {
	std::uint64_t expected_epoch = 0;
	std::string idempotency_key;
	std::string actor;
	std::string request_id;
	std::int64_t requested_at_nanoseconds = 0;
};

struct ResetResult {
	std::uint64_t epoch = 0;
	bool replayed = false;
};

/**
 * Meter-stream-owned authoritative energy and demand ledger.
 *
 * R5C1 counters are volatile per-session checkpoints. This class converts
 * them to durable lifetime counters, rejects rollback, applies Admin reset
 * epochs, and snapshots each completed ten-minute boundary under WAL with
 * synchronous=FULL.
 */
class EnergyLedger final {
public:
	explicit EnergyLedger(const std::filesystem::path &path);
	~EnergyLedger();
	EnergyLedger(const EnergyLedger &) = delete;
	EnergyLedger &operator=(const EnergyLedger &) = delete;

	[[nodiscard]] EnergyValues ingest_energy(const EnergyValues &session,
		std::uint32_t source_sequence,
		std::uint32_t configuration_generation,
		std::int64_t ingested_at_nanoseconds);
	[[nodiscard]] DemandValues ingest_demand(const DemandValues &session,
		std::uint32_t source_sequence,
		std::uint32_t configuration_generation,
		std::int64_t ingested_at_nanoseconds);

	[[nodiscard]] std::optional<EnergyValues> energy() const;
	[[nodiscard]] std::optional<DemandValues> demand() const;
	[[nodiscard]] ResetResult reset_energy(const ResetRequest &request);
	[[nodiscard]] ResetResult reset_demand_peaks(const ResetRequest &request);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace msap1::energy_ledger
