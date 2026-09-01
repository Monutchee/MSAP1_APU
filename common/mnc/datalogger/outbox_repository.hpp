#pragma once

#include "mnc/datalogger/data_channel.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <span>

namespace mnc::datalogger {

enum class DeliveryState : std::uint8_t {
	Pending,
	RetryWait,
	Blocked,
	Succeeded,
	AdministrativelyDiscarded,
	InFlight,
};

struct QueuedDelivery {
	std::string artifact_id;
	std::string channel_id;
	DeliveryState state = DeliveryState::Pending;
	std::uint32_t attempt_count = 0;
	UtcNanoseconds next_attempt = 0;
	UtcNanoseconds last_attempt = 0;
	std::string remote_result;
	std::string last_error;
};

enum class ArtifactState : std::uint8_t {
	Pending,
	PartiallyDelivered,
	Blocked,
	Succeeded,
	LocalOnly,
	MissingPayload,
};

struct ArtifactSummary {
	std::string artifact_id;
	std::string job_id;
	std::uint64_t job_revision = 0;
	std::string filename;
	std::string mime_type;
	std::string sha256;
	std::uint64_t size_bytes = 0;
	UtcWindow source_window;
	UtcNanoseconds generated_at = 0;
	UtcNanoseconds created_at = 0;
	ArtifactState state = ArtifactState::Pending;
	bool local_only = false;
	bool payload_present = false;
	std::uint32_t delivery_count = 0;
	std::uint32_t succeeded_count = 0;
	std::uint32_t blocked_count = 0;
	std::string recovery_error;
};

struct ArtifactDetail {
	ArtifactSummary artifact;
	std::vector<QueuedDelivery> deliveries;
};

inline constexpr std::size_t maximum_artifact_list_page = 500;

struct ArtifactListFilter {
	std::optional<std::string> job_id;
	std::optional<ArtifactState> state;
	std::optional<UtcNanoseconds> start;
	std::optional<UtcNanoseconds> end;
	std::size_t offset = 0;
	std::size_t limit = 100;
};

struct OutboxStoragePolicy {
	std::uint64_t maximum_bytes = 512ull * 1024ull * 1024ull;
	std::uint64_t minimum_free_bytes = 256ull * 1024ull * 1024ull;
	UtcNanoseconds completed_metadata_retention =
		30ll * 24ll * 60ll * 60ll * 1'000'000'000ll;
};

struct StorageGuardStatus {
	std::uint64_t payload_bytes = 0;
	std::uint64_t maximum_bytes = 0;
	std::uint64_t available_bytes = 0;
	std::uint64_t minimum_free_bytes = 0;
	bool generation_allowed = true;
	std::string blocking_reason;
};

struct OutboxStatus {
	std::uint64_t artifact_count = 0;
	std::uint64_t outbox_count = 0;
	std::uint64_t outbox_bytes = 0;
	std::uint64_t archive_count = 0;
	std::uint64_t archive_bytes = 0;
	std::uint64_t completed_metadata_count = 0;
	std::uint64_t missing_payload_count = 0;
	std::uint64_t pending_delivery_count = 0;
	std::uint64_t blocked_delivery_count = 0;
	std::optional<UtcNanoseconds> oldest_pending_created_at;
	StorageGuardStatus storage;
};

struct JobWatermark {
	std::string job_id;
	std::uint64_t job_revision = 0;
	UtcNanoseconds completed_through = 0;
};

struct ArtifactDeletionResult {
	std::uint64_t deleted = 0;
	std::uint64_t discarded_deliveries = 0;
};

struct ArtifactContentChunk {
	std::string artifact_id;
	std::string filename;
	std::string mime_type;
	std::string sha256;
	std::uint64_t total_size = 0;
	std::uint64_t offset = 0;
	std::string content;
	bool end_of_file = true;
};

class OutboxRepository {
public:
	virtual ~OutboxRepository() = default;
	virtual void initialize() = 0;
	virtual void update_storage_policy(OutboxStoragePolicy policy) = 0;
	virtual void enqueue(const GeneratedDataset &dataset,
		const GeneratedContent &content,
		std::span<const std::string> channel_ids, bool local_only) = 0;
	[[nodiscard]] virtual std::vector<QueuedDelivery> due(
		UtcNanoseconds now, std::size_t limit) const = 0;
	[[nodiscard]] virtual std::vector<QueuedDelivery> claim_due(
		UtcNanoseconds now, UtcNanoseconds lease_until,
		std::size_t limit) = 0;
	[[nodiscard]] virtual GeneratedContent content(
		std::string_view artifact_id) const = 0;
	virtual void record_result(const QueuedDelivery &delivery,
		const DeliveryResult &result, UtcNanoseconds attempted_at,
		UtcNanoseconds next_attempt) = 0;
	[[nodiscard]] virtual std::vector<ArtifactSummary> list(
		const ArtifactListFilter &filter) const = 0;
	[[nodiscard]] virtual ArtifactDetail artifact(
		std::string_view artifact_id) const = 0;
	[[nodiscard]] virtual std::string preview(std::string_view artifact_id,
		std::size_t maximum_bytes) const = 0;
	[[nodiscard]] virtual ArtifactContentChunk read_chunk(
		std::string_view artifact_id, std::uint64_t offset,
		std::size_t maximum_bytes) const = 0;
	virtual void retry(std::span<const std::string> artifact_ids,
		UtcNanoseconds now) = 0;
	virtual void retry_channel(std::string_view channel_id,
		UtcNanoseconds now) = 0;
	/** Reject removal of a channel still named by an incomplete delivery. */
	virtual void validate_channels(
		std::span<const std::string> available_channel_ids) const = 0;
	/** Remove delivered remote manifests older than the configured bound. */
	virtual void prune_completed(UtcNanoseconds now) = 0;
	[[nodiscard]] virtual ArtifactDeletionResult erase(
		std::span<const std::string> artifact_ids, bool discard_unsent,
		UtcNanoseconds now) = 0;
	[[nodiscard]] virtual OutboxStatus status() const = 0;
	[[nodiscard]] virtual std::optional<JobWatermark> watermark(
		std::string_view job_id) const = 0;
	virtual void store_watermark(const JobWatermark &watermark,
		UtcNanoseconds updated_at) = 0;
};

/** SQLite manifest plus crash-safe immutable payload files. */
class SqliteOutboxRepository final : public OutboxRepository {
public:
	SqliteOutboxRepository(std::filesystem::path root,
		OutboxStoragePolicy policy = {});
	~SqliteOutboxRepository() override;
	SqliteOutboxRepository(const SqliteOutboxRepository &) = delete;
	SqliteOutboxRepository &operator=(const SqliteOutboxRepository &) = delete;

	void initialize() override;
	void update_storage_policy(OutboxStoragePolicy policy) override;
	void enqueue(const GeneratedDataset &dataset,
		const GeneratedContent &content,
		std::span<const std::string> channel_ids, bool local_only) override;
	[[nodiscard]] std::vector<QueuedDelivery> due(
		UtcNanoseconds now, std::size_t limit) const override;
	[[nodiscard]] std::vector<QueuedDelivery> claim_due(
		UtcNanoseconds now, UtcNanoseconds lease_until,
		std::size_t limit) override;
	[[nodiscard]] GeneratedContent content(
		std::string_view artifact_id) const override;
	void record_result(const QueuedDelivery &delivery,
		const DeliveryResult &result, UtcNanoseconds attempted_at,
		UtcNanoseconds next_attempt) override;
	[[nodiscard]] std::vector<ArtifactSummary> list(
		const ArtifactListFilter &filter) const override;
	[[nodiscard]] ArtifactDetail artifact(
		std::string_view artifact_id) const override;
	[[nodiscard]] std::string preview(std::string_view artifact_id,
		std::size_t maximum_bytes) const override;
	[[nodiscard]] ArtifactContentChunk read_chunk(
		std::string_view artifact_id, std::uint64_t offset,
		std::size_t maximum_bytes) const override;
	void retry(std::span<const std::string> artifact_ids,
		UtcNanoseconds now) override;
	void retry_channel(std::string_view channel_id,
		UtcNanoseconds now) override;
	void validate_channels(
		std::span<const std::string> available_channel_ids) const override;
	void prune_completed(UtcNanoseconds now) override;
	[[nodiscard]] ArtifactDeletionResult erase(
		std::span<const std::string> artifact_ids, bool discard_unsent,
		UtcNanoseconds now) override;
	[[nodiscard]] OutboxStatus status() const override;
	[[nodiscard]] std::optional<JobWatermark> watermark(
		std::string_view job_id) const override;
	void store_watermark(const JobWatermark &watermark,
		UtcNanoseconds updated_at) override;

private:
	class Implementation;
	std::unique_ptr<Implementation> implementation_;
};

} // namespace mnc::datalogger
