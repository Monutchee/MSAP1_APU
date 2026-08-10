#pragma once

#include "mnc/MeterDataStreamer/database_policy.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mnc::meter_stream {

struct RecordTimingProvenance {
	std::uint64_t first_sample_index = 0;
	std::uint32_t sample_count = 0;
	std::uint32_t cycle_count = 0;
	std::uint8_t time_quality = 0;
	std::optional<std::int64_t> utc_start_nanoseconds;
	std::optional<std::uint64_t> utc_uncertainty_nanoseconds;
};

/** Product-neutral durable envelope around one exact producer record. */
struct MeterStreamRecord {
	std::uint64_t cursor = 0;
	std::uint32_t record_format = 0;
	std::uint16_t record_kind = 0;
	std::uint8_t measurement_period = 0;
	std::uint64_t source_sequence = 0;
	std::uint32_t configuration_generation = 0;
	std::int64_t ingested_at_nanoseconds = 0;
	RecordTimingProvenance timing{};
	std::vector<std::byte> payload;
};

struct StreamStatus {
	bool durability = true;
	std::uint64_t oldest_cursor = 0;
	std::uint64_t newest_cursor = 0;
	std::uint64_t record_count = 0;
	std::uint64_t storage_bytes = 0;
	struct ConsumerCursor {
		std::string name;
		std::uint64_t acknowledged_cursor = 0;
	};
	std::vector<ConsumerCursor> consumers;
};

class MeterRecordPublisher {
public:
	virtual ~MeterRecordPublisher() = default;
	/** Returns the committed ordered cursor. Retries are idempotent. */
	virtual std::uint64_t publish(const MeterStreamRecord &record) = 0;
};

class MeterStreamConsumer {
public:
	virtual ~MeterStreamConsumer() = default;
	virtual void register_consumer(std::string_view name) = 0;
	virtual void unregister_consumer(std::string_view name) = 0;
	virtual std::vector<MeterStreamRecord> read_after(
		std::string_view name, std::size_t limit) = 0;
	virtual void acknowledge(std::string_view name,
		std::uint64_t cursor) = 0;
};

/** SQLite-backed ordered spool with independent durable consumer cursors. */
class DurableMeterSpool final : public MeterRecordPublisher,
			       public MeterStreamConsumer {
public:
	DurableMeterSpool(std::filesystem::path persistent_path,
			  DatabaseStoragePolicy policy);
	~DurableMeterSpool();
	DurableMeterSpool(const DurableMeterSpool &) = delete;
	DurableMeterSpool &operator=(const DurableMeterSpool &) = delete;

	std::uint64_t publish(const MeterStreamRecord &record) override;
	void register_consumer(std::string_view name) override;
	void unregister_consumer(std::string_view name) override;
	std::vector<MeterStreamRecord> read_after(
		std::string_view name, std::size_t limit) override;
	void acknowledge(std::string_view name, std::uint64_t cursor) override;
	[[nodiscard]] StreamStatus status() const;
	[[nodiscard]] DatabaseStoragePolicy policy() const;
	void apply_policy(DatabaseStoragePolicy policy);
	void prune();

private:
	class Impl;
	mutable std::mutex lifecycle_mutex_;
	std::unique_ptr<Impl> impl_;
};

} // namespace mnc::meter_stream
