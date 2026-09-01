#include "mnc/datalogger/scheduler.hpp"

#include <filesystem>
#include <map>
#include <stdexcept>

#include <unistd.h>

namespace {

using namespace mnc::datalogger;
constexpr UtcNanoseconds second = 1'000'000'000ll;

void require(bool condition, std::string_view message)
{
	if (!condition)
		throw std::runtime_error(std::string(message));
}

class TestTree {
public:
	explicit TestTree(std::string_view name)
		: root(std::filesystem::temp_directory_path() /
			("m19-engine-" + std::string(name) + "-" +
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

class FakeClock final : public Clock {
public:
	UtcNanoseconds value = 0;
	UtcNanoseconds now() const noexcept override { return value; }
};

class FakeDatalogger final : public Datalogger {
public:
	mutable std::vector<UtcWindow> windows;
	mutable bool unavailable = false;
	std::optional<UtcNanoseconds> retained_from;

	GeneratedDataset generate(const DatalogJobSnapshot &job,
		UtcWindow window, UtcNanoseconds generated_at) const override
	{
		if (unavailable)
			throw DatalogError(DatalogErrorCode::SourceUnavailable,
				"historian unavailable");
		if (retained_from && window.start < *retained_from)
			throw DatalogError(DatalogErrorCode::SourceRetentionGap,
				"source window predates retained historian data");
		windows.push_back(window);
		GeneratedDataset result;
		result.artifact_id = job.job_id + "-r" +
			std::to_string(job.revision) + "-" +
			std::to_string(window.start) + "-" +
			std::to_string(window.end) + "-json";
		result.job_id = job.job_id;
		result.job_revision = job.revision;
		result.product_id = job.product_id;
		result.device_id = job.device_id;
		result.source_period = job.source_period;
		result.format = job.format;
		result.generated_at = generated_at;
		result.artifact_window = window;
		return result;
	}
};

ScheduledJob job(std::uint64_t revision = 1)
{
	ScheduledJob result;
	result.snapshot.job_id = "job-1";
	result.snapshot.revision = revision;
	result.snapshot.product_id = "msap1";
	result.snapshot.device_id = "device-1";
	result.snapshot.source_period = mnc::meter::MeasurementPeriod::Basic;
	result.snapshot.generation_interval_nanoseconds = 300 * second;
	result.snapshot.row_interval_nanoseconds = 60 * second;
	result.snapshot.format = ContentFormat::Json;
	result.snapshot.selections = {{
		.attribute = {mnc::meter::MeterAttributeId::VanRms, std::nullopt},
		.calculation = mnc::meter::MeterAttributeCalculation::Average}};
	result.enabled = true;
	result.channel_ids = {"channel-a", "channel-b"};
	return result;
}

void new_jobs_start_at_next_boundary_and_catch_up_in_order()
{
	TestTree tree("schedule");
	FakeClock clock;
	clock.value = 12 * 60 * second + 2 * second;
	FakeDatalogger datalogger;
	DefaultMeterDataContentWriterFactory writers;
	SqliteOutboxRepository outbox(tree.root,
		{16u * 1024u * 1024u, 0});
	outbox.initialize();
	DataSenderEngine engine(datalogger, writers, outbox, clock);
	engine.apply_jobs({job()});
	auto status = engine.job_status();
	require(status.size() == 1 && status.front().next_window ==
		UtcWindow{15 * 60 * second, 20 * 60 * second},
		"new job did not start at the next UTC boundary");
	clock.value = 20 * 60 * second + 29 * second;
	require(engine.generate_due().generated == 0,
		"scheduler generated before the historian settle delay");
	clock.value += second;
	require(engine.generate_due().generated == 1 &&
		datalogger.windows.front() ==
			UtcWindow{15 * 60 * second, 20 * 60 * second},
		"scheduler did not generate the first complete UTC window");
	clock.value = 31 * 60 * second;
	require(engine.generate_due(2).generated == 2 &&
		datalogger.windows[1] ==
			UtcWindow{20 * 60 * second, 25 * 60 * second} &&
		datalogger.windows[2] ==
			UtcWindow{25 * 60 * second, 30 * 60 * second},
		"restart/catch-up windows were not generated in order");
}

void source_failure_does_not_advance_the_watermark()
{
	TestTree tree("source");
	FakeClock clock;
	clock.value = 0;
	FakeDatalogger datalogger;
	DefaultMeterDataContentWriterFactory writers;
	SqliteOutboxRepository outbox(tree.root,
		{16u * 1024u * 1024u, 0});
	outbox.initialize();
	DataSenderEngine engine(datalogger, writers, outbox, clock);
	engine.apply_jobs({job()});
	clock.value = 330 * second;
	datalogger.unavailable = true;
	const auto deferred = engine.generate_due();
	require(deferred.generated == 0 && deferred.source_deferred &&
		outbox.watermark("job-1")->completed_through == 0,
		"historian outage advanced the durable generation watermark");
	datalogger.unavailable = false;
	require(engine.generate_due().generated == 1,
		"pending generation window did not resume after historian recovery");
}

void expired_source_windows_are_skipped_without_fabricated_artifacts()
{
	TestTree tree("retention");
	FakeClock clock;
	FakeDatalogger datalogger;
	DefaultMeterDataContentWriterFactory writers;
	SqliteOutboxRepository outbox(tree.root,
		{16u * 1024u * 1024u, 0});
	outbox.initialize();
	DataSenderEngine engine(datalogger, writers, outbox, clock);
	engine.apply_jobs({job()});
	datalogger.retained_from = 600 * second;
	clock.value = 930 * second;
	const auto catch_up = engine.generate_due(3);
	require(catch_up.skipped_expired == 2 && catch_up.generated == 1 &&
		!catch_up.source_deferred &&
		outbox.watermark("job-1")->completed_through == 900 * second,
		"expired source windows permanently blocked scheduler catch-up");
	require(datalogger.windows ==
		std::vector<UtcWindow>{{600 * second, 900 * second}},
		"scheduler queried or fabricated an expired source window");
	ArtifactListFilter filter;
	filter.limit = 10;
	const auto artifacts = outbox.list(filter);
	require(artifacts.size() == 1 &&
		artifacts.front().source_window ==
			UtcWindow{600 * second, 900 * second},
		"retention catch-up did not emit exactly the first retained window");
}

void configuration_revision_switches_at_a_future_boundary()
{
	TestTree tree("revision");
	FakeClock clock;
	clock.value = 122 * second;
	FakeDatalogger datalogger;
	DefaultMeterDataContentWriterFactory writers;
	SqliteOutboxRepository outbox(tree.root,
		{16u * 1024u * 1024u, 0});
	outbox.initialize();
	DataSenderEngine engine(datalogger, writers, outbox, clock);
	engine.apply_jobs({job(1)});
	clock.value = 181 * second;
	engine.apply_jobs({job(2)});
	const auto watermark = outbox.watermark("job-1");
	require(watermark && watermark->job_revision == 2 &&
		watermark->completed_through == 300 * second,
		"job edit reused a partial old-revision UTC window");
}

void successful_channels_are_not_resent_when_a_sibling_retries()
{
	TestTree tree("delivery");
	FakeClock clock;
	clock.value = 0;
	FakeDatalogger datalogger;
	DefaultMeterDataContentWriterFactory writers;
	SqliteOutboxRepository outbox(tree.root,
		{16u * 1024u * 1024u, 0});
	outbox.initialize();
	DataSenderEngine engine(datalogger, writers, outbox, clock);
	engine.apply_jobs({job()});
	clock.value = 330 * second;
	require(engine.generate_due().generated == 1,
		"delivery test artifact was not generated");
	std::map<std::string, std::uint32_t> attempts;
	auto executor = [&](std::string_view channel,
		const DeliveryRequest &) -> DeliveryResult {
		++attempts[std::string(channel)];
		if (channel == "channel-a")
			return {DeliveryDisposition::Succeeded, "204", {}};
		return attempts[std::string(channel)] == 1
			? DeliveryResult{DeliveryDisposition::Retryable, {}, "offline"}
			: DeliveryResult{DeliveryDisposition::Succeeded, "stored", {}};
	};
	require(engine.deliver_due(executor, 2) == 2,
		"multi-channel delivery batch was incomplete");
	ArtifactListFilter filter;
	filter.limit = 1;
	const auto queued = outbox.list(filter);
	const auto delay = DataSenderEngine::retry_delay(1,
		queued.front().artifact_id + ":channel-b");
	clock.value += delay;
	require(engine.deliver_due(executor, 2) == 1,
		"failed channel was not retried independently");
	require(attempts["channel-a"] == 1 && attempts["channel-b"] == 2,
		"successful sibling channel was sent again");
}

void local_only_never_calls_the_delivery_executor()
{
	TestTree tree("local");
	FakeClock clock;
	FakeDatalogger datalogger;
	DefaultMeterDataContentWriterFactory writers;
	SqliteOutboxRepository outbox(tree.root,
		{16u * 1024u * 1024u, 0});
	outbox.initialize();
	auto local = job();
	local.local_only = true;
	local.channel_ids.clear();
	DataSenderEngine engine(datalogger, writers, outbox, clock);
	engine.apply_jobs({local});
	clock.value = 330 * second;
	(void)engine.generate_due();
	bool called = false;
	const auto delivered = engine.deliver_due(
		[&](std::string_view, const DeliveryRequest &) {
			called = true;
			return DeliveryResult{};
		}, 1);
	require(delivered == 0 && !called,
		"Local-only job attempted a network delivery");
}

void empty_configuration_is_idle()
{
	TestTree tree("empty");
	FakeClock clock;
	clock.value = 24 * 60 * 60 * second;
	FakeDatalogger datalogger;
	DefaultMeterDataContentWriterFactory writers;
	SqliteOutboxRepository outbox(tree.root,
		{16u * 1024u * 1024u, 0});
	outbox.initialize();
	DataSenderEngine engine(datalogger, writers, outbox, clock);
	engine.apply_jobs({});
	bool transfer_called = false;
	const auto generation = engine.generate_due();
	const auto deliveries = engine.deliver_due(
		[&](std::string_view, const DeliveryRequest &) {
			transfer_called = true;
			return DeliveryResult{};
		}, 4);
	require(generation.generated == 0 && !generation.source_deferred &&
		!generation.storage_blocked && deliveries == 0 && !transfer_called &&
		datalogger.windows.empty() && engine.job_status().empty(),
		"empty factory configuration performed generation or transfer work");
}

} // namespace

int main()
{
	new_jobs_start_at_next_boundary_and_catch_up_in_order();
	source_failure_does_not_advance_the_watermark();
	expired_source_windows_are_skipped_without_fabricated_artifacts();
	configuration_revision_switches_at_a_future_boundary();
	successful_channels_are_not_resent_when_a_sibling_retries();
	local_only_never_calls_the_delivery_executor();
	empty_configuration_is_idle();
}
