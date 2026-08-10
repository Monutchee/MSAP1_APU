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

} // namespace

int main()
{
	spool_is_ordered_idempotent_and_durable();
	spool_backend_switch_replaces_stale_target();
	malformed_policies_are_rejected();
	historian_preserves_quality_and_storage_routing();
}
