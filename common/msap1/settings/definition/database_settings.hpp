#pragma once

#include "mnc/MeterDataProvider/stream/database_policy.hpp"

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace msap1::settings {

/** JSON representation of one dataset's storage and retention policy. */
struct DatasetStorageSettings {
	std::string backend = "persistent";
	/** Zero means forever. */
	std::uint64_t maximum_age_seconds = 0;
	/** Zero means no explicit size cap. */
	std::uint64_t maximum_bytes = 0;
	/** Required before selecting a volatile raw-record spool. */
	bool volatile_spool_acknowledged = false;

	void validate(bool spool) const
	{
		if (backend != "memory" && backend != "persistent")
			throw std::runtime_error("database backend must be memory or persistent");
		if (spool && backend == "memory" && !volatile_spool_acknowledged)
			throw std::runtime_error(
				"volatile spool requires an explicit warning acknowledgement");
	}
};

/**
 * Complete database policy. Defaults are product-safe and are also stated in
 * factory-defaults.json. Keeping member defaults makes older settings
 * documents acquire this section without losing unrelated configuration.
 */
struct DatabaseSettings {
	/*
	 * The spool is a handoff buffer, not an archive: every consumer projects
	 * its records within milliseconds under normal operation, and the durable
	 * measurement record lives in the historian projections. One hour is
	 * therefore ample, and it bounds two costs that a 24 h window made
	 * severe. It caps the volatile rebuild a restart has to replay, and it
	 * caps how much of this per-record write load is retained.
	 *
	 * The memory backend is the default because the producer's publish is a
	 * SYNCHRONOUS round-trip on the acquisition hot path: with a persistent
	 * spool every record costs an fsync on /data, and one slow-storage
	 * episode stalls acquisition long enough to overrun the kernel DMA ring
	 * and lose PL records (observed in the field on SD media). Volatility is
	 * safe because the cursor lease keeps cursors monotonic across restarts
	 * and the historian reports the lost rebuild window truthfully.
	 *
	 * Age pruning cannot lose data: prune() deletes only
	 * `cursor <= MIN(acknowledged_cursor) AND ingested_at_ns < cutoff`, so
	 * records no consumer has acknowledged are never removed however old they
	 * are. A lagging historian holds the prune point back and the spool grows
	 * past an hour until it catches up — which is why the byte cap exists: it
	 * is the hard bound that turns a wedged consumer into bounded, reported
	 * record loss instead of unbounded memory growth.
	 */
	DatasetStorageSettings spool{
		.backend = "memory", .maximum_age_seconds = 60u * 60u,
		.maximum_bytes = 32ull * 1024ull * 1024ull,
		.volatile_spool_acknowledged = true};
	DatasetStorageSettings basic{
		.backend = "memory", .maximum_age_seconds = 24u * 60u * 60u,
		.maximum_bytes = 512ull * 1024ull * 1024ull};
	DatasetStorageSettings cycles_150_180{};
	DatasetStorageSettings minutes_10{};
	DatasetStorageSettings hours_2{};

	void validate() const
	{
		spool.validate(true);
		basic.validate(false);
		cycles_150_180.validate(false);
		minutes_10.validate(false);
		hours_2.validate(false);
	}

	[[nodiscard]] std::vector<mnc::meter_stream::DatabaseStoragePolicy>
	policies() const
	{
		using namespace mnc::meter_stream;
		auto convert = [](DatabaseDataset dataset,
				  const DatasetStorageSettings &source) {
			DatabaseStoragePolicy result;
			result.dataset = dataset;
			result.backend = source.backend == "memory"
				? StorageBackend::memory : StorageBackend::persistent;
			if (source.maximum_age_seconds != 0)
				result.retention.maximum_age =
					std::chrono::seconds(source.maximum_age_seconds);
			if (source.maximum_bytes != 0)
				result.retention.maximum_bytes = source.maximum_bytes;
			return result;
		};
		return {
			convert(DatabaseDataset::raw_record_spool, spool),
			convert(DatabaseDataset::basic, basic),
			convert(DatabaseDataset::cycles_150_180, cycles_150_180),
			convert(DatabaseDataset::minutes_10, minutes_10),
			convert(DatabaseDataset::hours_2, hours_2),
		};
	}

	[[nodiscard]] mnc::meter_stream::DatabaseStoragePolicy
	spool_policy() const
	{
		return policies().front();
	}

	[[nodiscard]] std::vector<mnc::meter_stream::DatabaseStoragePolicy>
	historian_policies() const
	{
		auto result = policies();
		result.erase(result.begin());
		return result;
	}
};

} // namespace msap1::settings
