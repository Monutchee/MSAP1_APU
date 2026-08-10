#include "mnc/MeterDataStreamer/meter_stream.hpp"
#include "msap1/meter/history/meter_history.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <ranges>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {

void require(bool condition, const char *message)
{
	if (!condition) throw std::runtime_error(message);
}

std::filesystem::path temporary_database(std::string_view name)
{
	return std::filesystem::temp_directory_path() /
		("msap1-" + std::string(name) + "-" +
		 std::to_string(::getpid()) + ".sqlite3");
}

void remove_database(const std::filesystem::path &path)
{
	std::error_code error;
	std::filesystem::remove(path, error);
	std::filesystem::remove(path.string() + "-wal", error);
	std::filesystem::remove(path.string() + "-shm", error);
}

mnc::meter_stream::DatabaseStoragePolicy spool_policy(
	mnc::meter_stream::StorageBackend backend)
{
	return {mnc::meter_stream::DatabaseDataset::raw_record_spool, backend,
		{std::chrono::hours(24), std::nullopt}};
}

mnc::meter_stream::MeterStreamRecord record(std::uint64_t sequence)
{
	mnc::meter_stream::MeterStreamRecord result;
	result.record_format = 0x00010002;
	result.record_kind = 1;
	result.measurement_period = 0;
	result.source_sequence = sequence;
	result.configuration_generation = 7;
	result.ingested_at_nanoseconds = 1'000'000'000 +
		static_cast<std::int64_t>(sequence);
	result.timing.first_sample_index = sequence * 6400;
	result.timing.sample_count = 6400;
	result.timing.cycle_count = 12;
	result.payload.assign(256, std::byte{0x5a});
	return result;
}

void spool_is_ordered_idempotent_and_durable()
{
	const auto path = temporary_database("spool-test");
	remove_database(path);
	{
		mnc::meter_stream::DurableMeterSpool spool(path,
			spool_policy(mnc::meter_stream::StorageBackend::persistent));
		const auto first = spool.publish(record(1));
		require(first == spool.publish(record(1)),
			"duplicate publish allocated another cursor");
		const auto second = spool.publish(record(2));
		require(second == first + 1, "stream cursors are not ordered");
		spool.register_consumer("historian");
		spool.register_consumer("audit");
		require(spool.read_after("historian", 16).size() == 2,
			"consumer did not receive committed records");
		spool.acknowledge("historian", first);
		spool.prune();
		require(spool.status().record_count == 2,
			"unacknowledged consumer records were pruned");
		spool.apply_policy(spool_policy(
			mnc::meter_stream::StorageBackend::memory));
		require(!spool.status().durability &&
			spool.status().record_count == 2,
			"backend switch did not preserve stream state");
		spool.apply_policy(spool_policy(
			mnc::meter_stream::StorageBackend::persistent));
		require(spool.status().durability &&
			spool.status().record_count == 2,
			"persistent switch did not preserve stream state");
	}
	{
		mnc::meter_stream::DurableMeterSpool reopened(path,
			spool_policy(mnc::meter_stream::StorageBackend::persistent));
		require(reopened.read_after("historian", 16).size() == 1,
			"historian acknowledgement did not survive reopen");
		require(reopened.read_after("audit", 16).size() == 2,
			"consumer cursors were not independent");
	}
	remove_database(path);
}

void spool_backend_switch_replaces_stale_target()
{
	const auto path = temporary_database("spool-replace-test");
	remove_database(path);
	{
		mnc::meter_stream::DurableMeterSpool old_disk(path,
			spool_policy(mnc::meter_stream::StorageBackend::persistent));
		(void)old_disk.publish(record(1));
	}
	{
		/* A volatile spool starts a new stream and must replace, rather than
		 * merge with, an older persistent target when durability is enabled. */
		mnc::meter_stream::DurableMeterSpool live(path,
			spool_policy(mnc::meter_stream::StorageBackend::memory));
		(void)live.publish(record(9));
		live.apply_policy(spool_policy(
			mnc::meter_stream::StorageBackend::persistent));
		require(live.status().record_count == 1,
			"backend switch resurrected a stale persistent record");
		live.register_consumer("check");
		const auto records = live.read_after("check", 4);
		require(records.size() == 1 && records.front().source_sequence == 9,
			"backend switch did not publish the active stream exactly");
	}
	remove_database(path);
}

void malformed_policies_are_rejected()
{
	using namespace mnc::meter_stream;
	bool rejected = false;
	try {
		validate_database_policy({DatabaseDataset::basic,
			static_cast<StorageBackend>(99), {}});
	} catch (const std::invalid_argument &) {
		rejected = true;
	}
	require(rejected, "invalid storage backend was accepted");

	rejected = false;
	try {
		validate_database_policy({DatabaseDataset::basic,
			StorageBackend::memory, {std::chrono::seconds(0), std::nullopt}});
	} catch (const std::invalid_argument &) {
		rejected = true;
	}
	require(rejected, "zero retention duration was accepted");
}

msap1::MeterUpdate fundamental_update()
{
	msap1::MeterUpdate update;
	update.period = msap1::MeasurementPeriod::Basic;
	update.sequence = 44;
	update.configuration_generation = 8;
	update.fundamental.emplace();
	update.fundamental->voltage_ln.phase_a = {
		0, msap1::MeasurementQuality::valid, 44,
		std::chrono::system_clock::now(),
		{6400, std::chrono::milliseconds(200)}};
	update.fundamental->voltage_ln.phase_b.quality =
		msap1::MeasurementQuality::unavailable;
	return update;
}

void historian_preserves_quality_and_storage_routing()
{
	using D = mnc::meter_stream::DatabaseDataset;
	using B = mnc::meter_stream::StorageBackend;
	const auto path = temporary_database("history-test");
	remove_database(path);
	const std::vector<mnc::meter_stream::DatabaseStoragePolicy> policies{
		{D::basic, B::memory, {std::chrono::hours(24), 512ull << 20}},
		{D::cycles_150_180, B::persistent, {}},
		{D::minutes_10, B::persistent, {}},
		{D::hours_2, B::persistent, {}},
	};
	{
		msap1::history::MeterHistoryStore history(path, policies);
		history.append(fundamental_update(), 1, 2'000'000'000);
		msap1::history::HistoryQuery query;
		query.period = msap1::MeasurementPeriod::Basic;
		query.start_nanoseconds = 0;
		query.end_nanoseconds = 3'000'000'000;
		query.limit = 64;
		const auto points = history.query(query);
		const auto zero = std::ranges::find_if(points, [](const auto &point) {
			return point.attribute == mnc::meter::MeterAttributeId::VanRms;
		});
		const auto unavailable = std::ranges::find_if(points, [](const auto &point) {
			return point.attribute == mnc::meter::MeterAttributeId::VbnRms;
		});
		require(zero != points.end() && zero->value == 0 &&
			zero->quality == msap1::MeasurementQuality::valid,
			"valid electrical zero was lost in historian storage");
		require(unavailable != points.end() &&
			unavailable->quality == msap1::MeasurementQuality::unavailable,
			"unavailable quality was converted into a valid zero");
		const auto status = history.status();
		const auto basic = std::ranges::find_if(status.datasets, [](const auto &item) {
			return item.dataset == D::basic;
		});
		require(basic != status.datasets.end() && basic->backend == B::memory &&
			basic->block_count == 1,
			"basic history was not routed to volatile storage");
	}
	remove_database(path);
}

/*
 * Retention accounting. The per-period logical size is tracked incrementally so
 * append() no longer rescans the whole projection, and these are the ways that
 * accounting can go wrong: double-counting an idempotent replay, losing track
 * across a maintenance operation that empties the tables, and simply failing to
 * enforce either cap.
 */
void historian_enforces_retention_without_rescanning()
{
	using D = mnc::meter_stream::DatabaseDataset;
	using B = mnc::meter_stream::StorageBackend;
	const auto path = temporary_database("history-retention-test");
	remove_database(path);
	msap1::history::HistoryQuery query;
	query.period = msap1::MeasurementPeriod::Basic;
	query.start_nanoseconds = 0;
	query.end_nanoseconds = 1'000'000'000'000ll;
	query.limit = 50000;

	/* One block is 96 bytes plus 40 per reading, and every append writes all
	 * eight attributes: 416 bytes. A 900-byte cap therefore holds exactly two
	 * blocks, so from the third append onward the oldest must be evicted and
	 * exactly two must remain. A cap that evicted the whole selection batch
	 * instead of just the surplus would leave zero. */
	{
		const std::vector<mnc::meter_stream::DatabaseStoragePolicy> policies{
			{D::basic, B::memory, {std::nullopt, 900ull}},
			{D::cycles_150_180, B::persistent, {}},
			{D::minutes_10, B::persistent, {}},
			{D::hours_2, B::persistent, {}},
		};
		msap1::history::MeterHistoryStore history(path, policies);
		for (std::uint64_t cursor = 1; cursor <= 12; ++cursor)
			history.append(fundamental_update(), cursor,
				static_cast<std::int64_t>(cursor) * 1'000'000'000ll);
		const auto status = history.status();
		const auto basic = std::ranges::find_if(status.datasets,
			[](const auto &item) { return item.dataset == D::basic; });
		require(basic != status.datasets.end() && basic->block_count == 2,
			"the byte cap did not hold exactly the two blocks it allows");
		/* Whatever survived must be the NEWEST, not the oldest. */
		const auto points = history.query(query);
		require(!points.empty(), "the byte cap evicted everything");
		std::int64_t newest = 0;
		for (const auto &point : points)
			newest = std::max(newest, point.measured_at_nanoseconds);
		require(newest == 12'000'000'000ll,
			"the byte cap evicted the newest block instead of the oldest");
	}
	remove_database(path);

	/* An idempotent replay must not be counted twice. Re-appending the same
	 * stream cursors leaves the row set unchanged, so a cap that is not
	 * exceeded must not start evicting. */
	{
		const std::vector<mnc::meter_stream::DatabaseStoragePolicy> policies{
			{D::basic, B::memory, {std::nullopt, 100ull << 20}},
			{D::cycles_150_180, B::persistent, {}},
			{D::minutes_10, B::persistent, {}},
			{D::hours_2, B::persistent, {}},
		};
		msap1::history::MeterHistoryStore history(path, policies);
		for (std::uint64_t cursor = 1; cursor <= 5; ++cursor)
			history.append(fundamental_update(), cursor,
				static_cast<std::int64_t>(cursor) * 1'000'000'000ll);
		const auto before = history.query(query).size();
		for (std::uint64_t cursor = 1; cursor <= 5; ++cursor)
			history.append(fundamental_update(), cursor,
				static_cast<std::int64_t>(cursor) * 1'000'000'000ll);
		require(history.query(query).size() == before,
			"replaying committed cursors changed the projection");
	}
	remove_database(path);

	/* The age cap must evict by measured time, keeping only what is inside the
	 * window relative to the newest block. */
	{
		const std::vector<mnc::meter_stream::DatabaseStoragePolicy> policies{
			{D::basic, B::memory, {std::chrono::seconds(5), std::nullopt}},
			{D::cycles_150_180, B::persistent, {}},
			{D::minutes_10, B::persistent, {}},
			{D::hours_2, B::persistent, {}},
		};
		msap1::history::MeterHistoryStore history(path, policies);
		for (std::uint64_t cursor = 1; cursor <= 20; ++cursor)
			history.append(fundamental_update(), cursor,
				static_cast<std::int64_t>(cursor) * 1'000'000'000ll);
		for (const auto &point : history.query(query))
			require(point.measured_at_nanoseconds >= 15'000'000'000ll,
				"a block older than the age window survived");
		require(!history.query(query).empty(),
			"the age cap evicted blocks inside its own window");
	}
	remove_database(path);

	/* After a recreate the tables are empty. A size still cached from before
	 * would make the byte cap evict from an empty projection, so the first
	 * append afterwards must survive. */
	{
		const std::vector<mnc::meter_stream::DatabaseStoragePolicy> policies{
			{D::basic, B::persistent, {std::nullopt, 900ull}},
			{D::cycles_150_180, B::persistent, {}},
			{D::minutes_10, B::persistent, {}},
			{D::hours_2, B::persistent, {}},
		};
		msap1::history::MeterHistoryStore history(path, policies);
		for (std::uint64_t cursor = 1; cursor <= 6; ++cursor)
			history.append(fundamental_update(), cursor,
				static_cast<std::int64_t>(cursor) * 1'000'000'000ll);
		history.recreate_database(100);
		history.append(fundamental_update(), 101, 200'000'000'000ll);
		require(!history.query(query).empty(),
			"a stale cached size evicted the first block after a recreate");
	}
	remove_database(path);
}

void historian_maintenance_preserves_explicit_clear_boundary()
{
	using D = mnc::meter_stream::DatabaseDataset;
	using B = mnc::meter_stream::StorageBackend;
	const auto path = temporary_database("history-maintenance-test");
	remove_database(path);
	const std::vector<mnc::meter_stream::DatabaseStoragePolicy> policies{
		{D::basic, B::memory, {}},
		{D::cycles_150_180, B::persistent, {}},
		{D::minutes_10, B::persistent, {}},
		{D::hours_2, B::persistent, {}},
	};
	{
		msap1::history::MeterHistoryStore history(path, policies);
		auto basic = fundamental_update();
		history.append(basic, 1, 2'000'000'000);
		auto aggregate = fundamental_update();
		aggregate.period = msap1::MeasurementPeriod::Cycles150_180;
		aggregate.sequence = 45;
		history.append(aggregate, 2, 2'100'000'000);

		const std::array clear_basic{D::basic};
		history.clear_datasets(clear_basic, 2);
		msap1::history::HistoryQuery basic_query{
			.period = msap1::MeasurementPeriod::Basic,
			.attributes = {},
			.start_nanoseconds = 0,
			.end_nanoseconds = 4'000'000'000,
			.limit = 64,
		};
		require(history.query(basic_query).empty(),
			"cleared basic projection still returned data");
		/* Replaying a retained record below the persisted floor must not
		 * resurrect data that an administrator explicitly deleted. */
		history.append(basic, 1, 2'000'000'000);
		require(history.query(basic_query).empty(),
			"spool replay resurrected cleared basic data");
		history.append(basic, 3, 3'000'000'000);
		require(!history.query(basic_query).empty(),
			"new data above the clear boundary was discarded");

		history.recreate_database(3);
		require(history.query(basic_query).empty(),
			"database recreation retained volatile history");
		history.append(aggregate, 2, 2'100'000'000);
		msap1::history::HistoryQuery aggregate_query = basic_query;
		aggregate_query.period = msap1::MeasurementPeriod::Cycles150_180;
		require(history.query(aggregate_query).empty(),
			"database recreation replay floor was not applied");
	}
	{
		/* Reopen the database to prove that the administrative clear boundary
		 * is persistent metadata, not merely an in-process filter. */
		msap1::history::MeterHistoryStore history(path, policies);
		auto aggregate = fundamental_update();
		aggregate.period = msap1::MeasurementPeriod::Cycles150_180;
		aggregate.sequence = 45;
		history.append(aggregate, 2, 2'100'000'000);
		msap1::history::HistoryQuery query{
			.period = msap1::MeasurementPeriod::Cycles150_180,
			.attributes = {},
			.start_nanoseconds = 0,
			.end_nanoseconds = 4'000'000'000,
			.limit = 64,
		};
		require(history.query(query).empty(),
			"database recreation floor was lost after reopen");
	}
	remove_database(path);
}

} // namespace

int main()
{
	spool_is_ordered_idempotent_and_durable();
	spool_backend_switch_replaces_stale_target();
	malformed_policies_are_rejected();
	historian_preserves_quality_and_storage_routing();
	historian_enforces_retention_without_rescanning();
	historian_maintenance_preserves_explicit_clear_boundary();
}
