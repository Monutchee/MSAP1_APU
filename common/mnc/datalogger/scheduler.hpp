#pragma once

#include "mnc/datalogger/clock.hpp"
#include "mnc/datalogger/datalogger.hpp"
#include "mnc/datalogger/meter_data_content_writer.hpp"
#include "mnc/datalogger/outbox_repository.hpp"

#include <functional>
#include <mutex>
#include <optional>

namespace mnc::datalogger {

struct ScheduledJob {
	DatalogJobSnapshot snapshot;
	bool enabled = false;
	bool local_only = false;
	std::vector<std::string> channel_ids;
};

struct ScheduledJobStatus {
	std::string job_id;
	std::uint64_t revision = 0;
	bool enabled = false;
	std::optional<UtcWindow> next_window;
	std::optional<UtcWindow> last_generated_window;
	UtcNanoseconds last_generated_at = 0;
	std::string last_error;
};

struct GenerationRunResult {
	std::size_t generated = 0;
	bool storage_blocked = false;
	bool source_deferred = false;
};

using DeliveryExecutor = std::function<DeliveryResult(
	std::string_view channel_id, const DeliveryRequest &request)>;

/**
 * Reusable UTC scheduler/retry engine. Process lifecycle and product settings
 * remain outside this class; generation and delivery dependencies are
 * injected so restart/catch-up behavior can be tested deterministically.
 */
class DataSenderEngine final {
public:
	DataSenderEngine(const Datalogger &datalogger,
		const MeterDataContentWriterFactory &writers,
		OutboxRepository &outbox, const Clock &clock);

	void apply_jobs(std::vector<ScheduledJob> jobs);
	[[nodiscard]] GenerationRunResult generate_due(
		std::size_t maximum_windows = 4);
	[[nodiscard]] std::size_t deliver_due(const DeliveryExecutor &deliver,
		std::size_t maximum_deliveries = 1);
	[[nodiscard]] std::vector<ScheduledJobStatus> job_status() const;

	[[nodiscard]] static UtcNanoseconds retry_delay(
		std::uint32_t attempt_number, std::string_view stable_identity) noexcept;

private:
	const Datalogger &datalogger_;
	const MeterDataContentWriterFactory &writers_;
	OutboxRepository &outbox_;
	const Clock &clock_;
	mutable std::mutex jobs_mutex_;
	std::vector<ScheduledJob> jobs_;
	std::vector<ScheduledJobStatus> statuses_;
};

class SystemClock final : public Clock {
public:
	[[nodiscard]] UtcNanoseconds now() const noexcept override;
};

} // namespace mnc::datalogger
