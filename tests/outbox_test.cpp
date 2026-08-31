#include "mnc/datalogger/meter_data_content_writer.hpp"
#include "mnc/datalogger/outbox_repository.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include <unistd.h>

namespace {

using namespace mnc::datalogger;

void require(bool condition, std::string_view message)
{
	if (!condition)
		throw std::runtime_error(std::string(message));
}

class TestTree {
public:
	explicit TestTree(std::string_view name)
		: root(std::filesystem::temp_directory_path() /
			("m19-outbox-" + std::string(name) + "-" +
			 std::to_string(::getpid())))
	{
		std::error_code ignored;
		std::filesystem::remove_all(root, ignored);
	}
	~TestTree()
	{
		std::error_code ignored;
		std::filesystem::remove_all(root, ignored);
	}

	std::filesystem::path root;
};

GeneratedDataset dataset(std::string id, UtcNanoseconds generated_at = 500)
{
	GeneratedDataset result;
	result.artifact_id = std::move(id);
	result.job_id = "job-1";
	result.job_revision = 3;
	result.product_id = "msap1";
	result.device_id = "device-1";
	result.source_period = mnc::meter::MeasurementPeriod::Basic;
	result.format = ContentFormat::Json;
	result.generated_at = generated_at;
	result.artifact_window = {0, 300'000'000'000ll};
	return result;
}

GeneratedContent content(const GeneratedDataset &value)
{
	return JsonMeterDataContentWriter{}.write(value);
}

void remote_delivery_is_independently_durable()
{
	TestTree tree("delivery");
	const auto data = dataset("job-1-r3-0-300-json");
	const auto body = content(data);
	const std::vector<std::string> channels{"channel-a", "channel-b"};
	{
		SqliteOutboxRepository repository(tree.root,
			{16u * 1024u * 1024u, 0});
		repository.initialize();
		repository.enqueue(data, body, channels, false);
		repository.enqueue(data, body, channels, false); // idempotent restart
		const auto queued_status = repository.status();
		require(queued_status.artifact_count == 1 &&
			queued_status.outbox_count == 1 &&
			queued_status.completed_metadata_count == 0,
			"idempotent enqueue duplicated the artifact");
		auto due = repository.due(500, 10);
		require(due.size() == 2, "each selected channel needs a delivery row");
		repository.record_result(due[0],
			{DeliveryDisposition::Succeeded, "204", {}}, 600, 0);
		const auto partial = repository.artifact(data.artifact_id);
		require(partial.artifact.state == ArtifactState::PartiallyDelivered &&
			partial.artifact.payload_present,
			"one successful channel removed a multi-channel payload");
		repository.record_result(due[1],
			{DeliveryDisposition::Retryable, {}, "network unavailable"},
			600, 5'000);
		require(repository.due(4'999, 10).empty(),
			"retry became eligible before its durable deadline");
	}
	{
		SqliteOutboxRepository recovered(tree.root,
			{16u * 1024u * 1024u, 0});
		recovered.initialize();
		auto due = recovered.due(5'000, 10);
		require(due.size() == 1 && due.front().channel_id == "channel-b" &&
			due.front().attempt_count == 1,
			"restart reset or duplicated delivery attempts");
		recovered.record_result(due.front(),
			{DeliveryDisposition::Succeeded, "stored", {}}, 5'100, 0);
		const auto complete = recovered.artifact(data.artifact_id);
		require(complete.artifact.state == ArtifactState::Succeeded &&
			!complete.artifact.payload_present,
			"payload was not removed after every durable acknowledgement");
		const auto completed_status = recovered.status();
		require(completed_status.artifact_count == 1 &&
			completed_status.outbox_count == 0 &&
			completed_status.completed_metadata_count == 1,
			"completed metadata was counted as a queued payload");
		require(!std::filesystem::exists(tree.root / "outbox" / body.filename),
			"completed remote payload remains in the outbox");
	}
}

void local_only_is_archived_until_explicit_deletion()
{
	TestTree tree("local");
	const auto data = dataset("local-job-r1-0-300-json");
	const auto body = content(data);
	SqliteOutboxRepository repository(tree.root,
		{16u * 1024u * 1024u, 0});
	repository.initialize();
	repository.enqueue(data, body, {}, true);
	require(repository.due(1000, 10).empty(),
		"Local-only artifact created a network delivery");
	const auto item = repository.artifact(data.artifact_id);
	require(item.artifact.state == ArtifactState::LocalOnly &&
		item.artifact.payload_present,
		"Local-only artifact was not retained in the archive");
	const auto local_status = repository.status();
	require(local_status.artifact_count == 1 &&
		local_status.archive_count == 1 && local_status.outbox_count == 0,
		"Local-only payload status was not separated from the outbox");
	require(repository.preview(data.artifact_id, 64).starts_with("{\n"),
		"archive preview did not read the manifest-authorized payload");
	const std::vector<std::string> ids{data.artifact_id};
	const auto deleted = repository.erase(ids, false, 2000);
	require(deleted.deleted == 1 && deleted.discarded_deliveries == 0,
		"Local-only deletion reported an unexpected discard");
}

void unsent_deletion_requires_explicit_discard()
{
	TestTree tree("discard");
	const auto data = dataset("remote-job-r1-0-300-json");
	const auto body = content(data);
	const std::vector<std::string> channels{"channel-a"};
	const std::vector<std::string> ids{data.artifact_id};
	SqliteOutboxRepository repository(tree.root,
		{16u * 1024u * 1024u, 0});
	repository.initialize();
	repository.enqueue(data, body, channels, false);
	bool refused = false;
	try {
		(void)repository.erase(ids, false, 1000);
	} catch (const std::runtime_error &) {
		refused = true;
	}
	require(refused, "unsent artifact deletion did not require confirmation");
	const auto deleted = repository.erase(ids, true, 1001);
	require(deleted.deleted == 1 && deleted.discarded_deliveries == 1,
		"confirmed unsent deletion did not record the discarded delivery");
}

void quota_guard_preserves_existing_payloads()
{
	TestTree tree("quota");
	const auto first = dataset("quota-job-r1-0-300-json");
	const auto first_body = content(first);
	const auto second = dataset("quota-job-r1-300-600-json", 600);
	const auto second_body = content(second);
	const std::vector<std::string> channels{"channel-a"};
	SqliteOutboxRepository repository(tree.root,
		{static_cast<std::uint64_t>(first_body.body.size() + 8), 0});
	repository.initialize();
	repository.enqueue(first, first_body, channels, false);
	bool blocked = false;
	try {
		repository.enqueue(second, second_body, channels, false);
	} catch (const DatalogError &error) {
		blocked = error.code() == DatalogErrorCode::StorageFailure;
	}
	require(blocked, "quota guard accepted an artifact beyond its limit");
	require(repository.content(first.artifact_id).sha256 == first_body.sha256,
		"quota guard damaged the existing unsent artifact");
}

void startup_reconciles_crash_windows_without_false_success()
{
	TestTree tree("recovery");
	std::filesystem::create_directories(tree.root / "outbox");
	std::filesystem::create_directories(tree.root / "archive");
	std::ofstream(tree.root / "outbox" / "orphan.json") << "{}\n";
	std::ofstream(tree.root / "archive" / "local-orphan.csv") << "a,b\r\n";
	std::ofstream(tree.root / "outbox" / ".partial.json.tmp.1") << "partial";
	{
		SqliteOutboxRepository repository(tree.root,
			{16u * 1024u * 1024u, 0});
		repository.initialize();
		ArtifactListFilter filter;
		filter.limit = 10;
		const auto items = repository.list(filter);
		require(items.size() == 2,
			"orphan recovery did not manifest both complete payloads");
		require(std::ranges::any_of(items, [](const auto &item) {
			return !item.local_only && item.state == ArtifactState::Blocked;
		}), "orphan outbox payload was incorrectly treated as delivered");
		require(std::ranges::any_of(items, [](const auto &item) {
			return item.local_only && item.state == ArtifactState::LocalOnly;
		}), "orphan archive payload was not retained as local data");
		require(!std::filesystem::exists(
			tree.root / "outbox" / ".partial.json.tmp.1"),
			"incomplete temporary file survived recovery");
	}

	TestTree missing("missing");
	const auto data = dataset("missing-job-r1-0-300-json");
	const auto body = content(data);
	const std::vector<std::string> channels{"channel-a"};
	{
		SqliteOutboxRepository repository(missing.root,
			{16u * 1024u * 1024u, 0});
		repository.initialize();
		repository.enqueue(data, body, channels, false);
	}
	std::filesystem::remove(missing.root / "outbox" / body.filename);
	{
		SqliteOutboxRepository repository(missing.root,
			{16u * 1024u * 1024u, 0});
		repository.initialize();
		const auto recovered = repository.artifact(data.artifact_id);
		require(recovered.artifact.state == ArtifactState::MissingPayload &&
			recovered.deliveries.front().state == DeliveryState::Blocked,
			"missing payload was marked successful during recovery");
		require(repository.status().missing_payload_count == 1,
			"missing payload was absent from service status");
	}
}

void startup_blocks_a_manifest_with_a_changed_payload_size()
{
	TestTree tree("size-mismatch");
	const auto data = dataset("size-mismatch-r1-0-300-json");
	const auto body = content(data);
	const std::vector<std::string> channels{"channel-a"};
	{
		SqliteOutboxRepository repository(tree.root,
			{16u * 1024u * 1024u, 0});
		repository.initialize();
		repository.enqueue(data, body, channels, false);
	}
	const auto path = tree.root / "outbox" / body.filename;
	std::filesystem::permissions(path, std::filesystem::perms::owner_write,
		std::filesystem::perm_options::add);
	std::ofstream(path, std::ios::binary | std::ios::trunc) << "short";
	SqliteOutboxRepository recovered(tree.root,
		{16u * 1024u * 1024u, 0});
	recovered.initialize();
	const auto item = recovered.artifact(data.artifact_id);
	require(item.artifact.state == ArtifactState::MissingPayload &&
		item.artifact.recovery_error == "payload size mismatch" &&
		item.deliveries.front().state == DeliveryState::Blocked,
		"changed payload size did not block delivery during startup recovery");
	require(recovered.status().missing_payload_count == 1,
		"damaged payload was absent from service status");
}

void automatic_channel_retry_preserves_durable_backoff()
{
	TestTree tree("channel-retry");
	const auto data = dataset("channel-retry-r1-0-300-json");
	const auto body = content(data);
	const std::vector<std::string> channels{"channel-a"};
	SqliteOutboxRepository repository(tree.root,
		{16u * 1024u * 1024u, 0});
	repository.initialize();
	repository.enqueue(data, body, channels, false);
	const auto delivery = repository.due(1000, 1).front();
	repository.record_result(delivery,
		{DeliveryDisposition::Retryable, {}, "temporary outage"}, 1100, 5000);
	repository.retry_channel("channel-a", 1200);
	require(repository.due(4999, 1).empty(),
		"configuration refresh collapsed a durable retry deadline");
	repository.retry(std::vector<std::string>{data.artifact_id}, 1300);
	require(repository.due(1300, 1).size() == 1,
		"explicit administrator retry did not bypass backoff");
}

void queued_channel_references_protect_configuration()
{
	TestTree tree("channel-reference");
	const auto data = dataset("channel-reference-r1-0-300-json");
	const auto body = content(data);
	const std::vector<std::string> channels{"channel-a", "channel-b"};
	SqliteOutboxRepository repository(tree.root,
		{16u * 1024u * 1024u, 0});
	repository.initialize();
	repository.enqueue(data, body, channels, false);
	repository.validate_channels(channels);
	bool refused = false;
	try {
		repository.validate_channels(
			std::vector<std::string>{"channel-a"});
	} catch (const std::runtime_error &) {
		refused = true;
	}
	require(refused,
		"configuration removed a channel referenced by queued delivery state");
	for (const auto &delivery : repository.due(1000, 10))
		repository.record_result(delivery,
			{DeliveryDisposition::Succeeded, "stored", {}}, 1100, 0);
	repository.validate_channels(std::span<const std::string>{});
}

void retry_rejects_an_unknown_artifact()
{
	TestTree tree("retry-unknown");
	SqliteOutboxRepository repository(tree.root,
		{16u * 1024u * 1024u, 0});
	repository.initialize();
	bool rejected = false;
	try {
		repository.retry(std::vector<std::string>{"missing-artifact"}, 1000);
	} catch (const std::out_of_range &) {
		rejected = true;
	}
	require(rejected, "retry silently accepted an unknown artifact ID");
}

void completed_metadata_retention_never_prunes_local_archives()
{
	TestTree tree("metadata-retention");
	const auto remote = dataset("retained-remote-r1-0-300-json");
	const auto local = dataset("retained-local-r1-0-300-json");
	const auto remote_body = content(remote);
	const auto local_body = content(local);
	const std::vector<std::string> channels{"channel-a"};
	SqliteOutboxRepository repository(tree.root,
		{16u * 1024u * 1024u, 0, 100});
	repository.initialize();
	repository.enqueue(remote, remote_body, channels, false);
	repository.enqueue(local, local_body, {}, true);
	repository.record_result(repository.due(1000, 1).front(),
		{DeliveryDisposition::Succeeded, "stored", {}}, 1000, 0);
	repository.prune_completed(1099);
	require(repository.artifact(remote.artifact_id).artifact.state ==
		ArtifactState::Succeeded,
		"completed metadata was pruned before its retention deadline");
	repository.prune_completed(1100);
	bool remote_pruned = false;
	try {
		(void)repository.artifact(remote.artifact_id);
	} catch (const std::out_of_range &) {
		remote_pruned = true;
	}
	require(remote_pruned,
		"completed remote metadata survived beyond its retention deadline");
	require(repository.artifact(local.artifact_id).artifact.state ==
		ArtifactState::LocalOnly,
		"metadata retention incorrectly pruned a Local-only archive");
}

void watermark_is_durable()
{
	TestTree tree("watermark");
	{
		SqliteOutboxRepository repository(tree.root,
			{16u * 1024u * 1024u, 0});
		repository.initialize();
		repository.store_watermark({"job-1", 7, 123456}, 123500);
	}
	SqliteOutboxRepository repository(tree.root,
		{16u * 1024u * 1024u, 0});
	repository.initialize();
	const auto value = repository.watermark("job-1");
	require(value && value->job_revision == 7 &&
		value->completed_through == 123456,
		"generation watermark did not survive restart");
}

} // namespace

int main()
{
	remote_delivery_is_independently_durable();
	local_only_is_archived_until_explicit_deletion();
	unsent_deletion_requires_explicit_discard();
	quota_guard_preserves_existing_payloads();
	startup_reconciles_crash_windows_without_false_success();
	startup_blocks_a_manifest_with_a_changed_payload_size();
	automatic_channel_retry_preserves_durable_backoff();
	queued_channel_references_protect_configuration();
	retry_rejects_an_unknown_artifact();
	completed_metadata_retention_never_prunes_local_archives();
	watermark_is_durable();
}
