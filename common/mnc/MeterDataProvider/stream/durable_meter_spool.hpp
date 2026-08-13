#pragma once

#include "mnc/MeterDataProvider/stream/database_policy.hpp"
#include "mnc/MeterDataProvider/stream/meter_record_publisher.hpp"
#include "mnc/MeterDataProvider/stream/meter_stream_consumer.hpp"
#include "mnc/MeterDataProvider/stream/meter_stream_status.hpp"

#include <filesystem>
#include <memory>
#include <mutex>

namespace mnc::meter_stream {

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
