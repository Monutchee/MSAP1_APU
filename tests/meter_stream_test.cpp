#include "mnc/MeterDataProvider/stream/meter_stream.hpp"
#include "mnc/storage/sqlite/sqlite_database.hpp"
#include "msap1/meter/MeterDataProvider/stream/meter_stream_ipc.hpp"
#include "msap1/meter/harmonic_spectrum.hpp"
#include "msap1/meter/history/meter_history.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <string>
#include <vector>
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
	std::filesystem::remove(path.string() + ".cursor-lease", error);
	std::filesystem::remove(path.string() + ".cursor-lease.tmp", error);
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
	result.record_format = 0x00010003;
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

msap1::MeterRecord harmonic_aggregate_record(std::uint32_t sequence,
	std::uint8_t channel, std::uint8_t chunk)
{
	msap1::MeterRecord result{};
	result.words[0] = msap1::meter_record_magic;
	result.words[1] = msap1::meter_harmonic_aggregate_format;
	result.words[2] = msap1::meter_record_size;
	result.words[3] = sequence;
	result.words[4] = 9u;
	result.words[5] = 32000u;
	result.words[6] = 96000u;
	result.words[7] = 0x7fu;
	result.words[8] = 0x3eu;
	result.words[9] = 0x1000u;
	result.words[11] = 0x18700u;
	const auto first_order = static_cast<std::uint8_t>(
		chunk * msap1::harmonic_aggregate_orders_per_record + 1u);
	const auto count = static_cast<std::uint8_t>(std::min<std::size_t>(
		msap1::harmonic_aggregate_orders_per_record,
		msap1::harmonic_max_order - first_order + 1u));
	result.words[13] = channel |
		(static_cast<std::uint32_t>(chunk) << 3) |
		(static_cast<std::uint32_t>(first_order) << 7) |
		(static_cast<std::uint32_t>(count) << 15) |
		(static_cast<std::uint32_t>(
			msap1::harmonic_chunks_per_channel) << 20) |
		(static_cast<std::uint32_t>(msap1::harmonic_max_order) << 24);
	result.words[14] = static_cast<std::uint32_t>(
		mnc::meter::MeasurementPeriod::Cycles150_180) |
		(15u << 2) | (1u << 30);
	result.words[15] = 127u | (50u << 8) | (10u << 16) | (3u << 24);
	for (std::size_t index = 0; index < count; ++index) {
		const auto order = first_order + index;
		const std::uint64_t packed =
			(static_cast<std::uint64_t>(channel) * 1000000u + order) |
			(std::uint64_t{1} << 60);
		result.words[16 + index * 2] = static_cast<std::uint32_t>(packed);
		result.words[17 + index * 2] =
			static_cast<std::uint32_t>(packed >> 32);
	}
	result.words[62] = 100u;
	result.words[63] = 114u;
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

void spool_distinguishes_source_fragments()
{
	const auto path = temporary_database("spool-fragment-test");
	remove_database(path);
	mnc::meter_stream::DurableMeterSpool spool(path,
		spool_policy(mnc::meter_stream::StorageBackend::memory));
	auto first = record(42);
	auto last = first;
	last.source_fragment = 41;
	last.payload.front() = std::byte{0x41};
	const auto first_cursor = spool.publish(first);
	const auto last_cursor = spool.publish(last);
	require(last_cursor == first_cursor + 1,
		"a distinct source fragment was treated as an idempotent replay");
	require(spool.publish(last) == last_cursor,
		"a replay of one source fragment allocated another cursor");
	spool.register_consumer("fragment-check");
	const auto committed = spool.read_after("fragment-check", 4);
	require(committed.size() == 2 &&
			committed[0].source_fragment == 0 &&
			committed[1].source_fragment == 41,
		"source fragments did not survive durable-envelope storage");

	const auto encoded = msap1::meter_stream::encode_record(last);
	mnc::ipc::ByteReader reader(encoded);
	const auto decoded = msap1::meter_stream::decode_record(reader);
	reader.require_finished();
	require(decoded.source_fragment == 41 &&
			decoded.source_sequence == last.source_sequence,
		"source fragment did not survive meter-stream IPC");
	remove_database(path);
}

void spool_atomically_publishes_record_families()
{
	const auto path = temporary_database("spool-family-test");
	remove_database(path);
	mnc::meter_stream::DurableMeterSpool spool(path,
		spool_policy(mnc::meter_stream::StorageBackend::memory));

	std::vector<mnc::meter_stream::MeterStreamRecord> family;
	for (std::uint16_t fragment = 0; fragment < 42; ++fragment) {
		auto member = record(77);
		member.source_fragment = fragment;
		member.timing.first_sample_index = 77 * 6400;
		family.push_back(std::move(member));
	}
	auto malformed = family;
	malformed[20].timing.utc_uncertainty_nanoseconds =
		std::numeric_limits<std::uint64_t>::max();
	bool rejected = false;
	try {
		(void)spool.publish_records(malformed);
	} catch (const std::overflow_error &) {
		rejected = true;
	}
	require(rejected && spool.status().record_count == 0,
		"a failed family transaction left partially committed records");

	const auto cursors = spool.publish_records(family);
	require(cursors.size() == family.size(),
		"family publish returned the wrong cursor count");
	for (std::size_t index = 1; index < cursors.size(); ++index)
		require(cursors[index] == cursors.front() + index,
			"family records did not receive contiguous ordered cursors");
	const auto replay = spool.publish_records(family);
	require(replay == cursors && spool.status().record_count == family.size(),
		"idempotent family replay changed the durable stream");

	const auto encoded = msap1::meter_stream::encode_records(family);
	mnc::ipc::ByteReader reader(encoded);
	const auto decoded = msap1::meter_stream::decode_records(reader);
	reader.require_finished();
	require(decoded.size() == family.size() &&
			decoded.back().source_fragment == 41,
		"record family did not round trip through meter-stream IPC");
	remove_database(path);
}

void spool_age_pruning_is_indexed_and_preserves_fresh_records()
{
	using namespace mnc::meter_stream;
	const auto path = temporary_database("spool-age-index-test");
	remove_database(path);
	{
		DatabaseStoragePolicy policy{DatabaseDataset::raw_record_spool,
			StorageBackend::persistent,
			{std::chrono::hours(1), std::nullopt}};
		DurableMeterSpool spool(path, policy);
		spool.register_consumer("historian");
		const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
		auto expired = record(1);
		expired.ingested_at_nanoseconds = now -
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::hours(2)).count();
		auto fresh = record(2);
		fresh.ingested_at_nanoseconds = now;
		const std::array family{expired, fresh};
		const auto cursors = spool.publish_records(family);
		spool.acknowledge("historian", cursors.back());
		spool.prune();
		const auto status = spool.status();
		require(status.record_count == 1,
			"age prune did not remove exactly the expired record");
		spool.register_consumer("historian-audit");
		const auto survivors = spool.read_after("historian-audit", 4);
		require(survivors.size() == 1 &&
			survivors.front().source_sequence == fresh.source_sequence,
			"age prune removed the fresh record");
	}
	{
		mnc::storage::sqlite::Database database(path);
		bool found_index = false;
		auto indexes = database.prepare("PRAGMA index_list(records)");
		while (indexes.step())
			found_index = found_index ||
				indexes.text(1) == "records_retention_age";
		require(found_index,
			"spool age-retention index was not created");

		bool indexed_plan = false;
		auto plan = database.prepare(
			"EXPLAIN QUERY PLAN SELECT COALESCE("
			"SUM(LENGTH(payload)+128),0) FROM records "
			"INDEXED BY records_retention_age "
			"WHERE cursor<=? AND ingested_at_ns<?");
		plan.bind(1, std::numeric_limits<std::int64_t>::max());
		plan.bind(2, std::numeric_limits<std::int64_t>::max());
		while (plan.step())
			indexed_plan = indexed_plan ||
				plan.text(3).find("records_retention_age") !=
					std::string::npos;
		require(indexed_plan,
			"age-prune query plan does not use the retention index");
	}
	remove_database(path);
}

void spool_migrates_legacy_identity_key()
{
	const auto path = temporary_database("spool-fragment-migration-test");
	remove_database(path);
	{
		mnc::storage::sqlite::Database legacy(path);
		legacy.execute(R"SQL(
CREATE TABLE records(
 cursor INTEGER PRIMARY KEY AUTOINCREMENT,
 record_format INTEGER NOT NULL,
 record_kind INTEGER NOT NULL,
 measurement_period INTEGER NOT NULL,
 source_sequence INTEGER NOT NULL,
 configuration_generation INTEGER NOT NULL,
 ingested_at_ns INTEGER NOT NULL,
 first_sample_index INTEGER NOT NULL,
 sample_count INTEGER NOT NULL,
 cycle_count INTEGER NOT NULL,
 time_quality INTEGER NOT NULL,
 utc_start_ns INTEGER NOT NULL,
 utc_uncertainty_ns INTEGER NOT NULL,
 payload BLOB NOT NULL,
 UNIQUE(record_format, configuration_generation, source_sequence,
        first_sample_index)
);
CREATE TABLE consumers(
 name TEXT PRIMARY KEY,
 acknowledged_cursor INTEGER NOT NULL DEFAULT 0,
 updated_at_ns INTEGER NOT NULL
);
INSERT INTO records(
 record_format,record_kind,measurement_period,source_sequence,
 configuration_generation,ingested_at_ns,first_sample_index,sample_count,
 cycle_count,time_quality,utc_start_ns,utc_uncertainty_ns,payload)
VALUES(65539,1,0,7,7,1000000007,44800,6400,12,0,
 -9223372036854775808,-9223372036854775808,X'5A');
)SQL");
	}
	{
		mnc::meter_stream::DurableMeterSpool spool(path,
			spool_policy(mnc::meter_stream::StorageBackend::persistent));
		spool.register_consumer("migration-check");
		const auto migrated = spool.read_after("migration-check", 4);
		require(migrated.size() == 1 &&
				migrated.front().source_sequence == 7 &&
				migrated.front().source_fragment == 0,
			"legacy spool rows did not migrate with fragment zero");
		auto fragment = record(7);
		fragment.source_fragment = 1;
		(void)spool.publish(fragment);
		require(spool.status().record_count == 2,
			"migrated identity key still collapsed distinct fragments");
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

void memory_spool_cursors_survive_restart()
{
	using namespace mnc::meter_stream;
	const auto path = temporary_database("spool-lease-test");
	remove_database(path);
	std::uint64_t first_cursor = 0;
	std::uint64_t first_session = 0;
	{
		DurableMeterSpool spool(path,
			spool_policy(StorageBackend::memory));
		first_cursor = spool.publish(record(1));
		first_session = spool.status().session_start_cursor;
		require(first_cursor > first_session,
			"published cursor is not inside its session");
		require(std::filesystem::exists(
				std::filesystem::path(path.string() + ".cursor-lease")),
			"spool construction did not persist a cursor lease");
	}
	{
		/* A fresh :memory: database restarts AUTOINCREMENT at 1; the lease
		 * must keep the public cursor space monotonic anyway, because
		 * consumers persist dedup keys and clear floors keyed by it. */
		DurableMeterSpool spool(path,
			spool_policy(StorageBackend::memory));
		require(spool.status().session_start_cursor > first_cursor,
			"session start did not advance past the previous session");
		require(spool.publish(record(2)) > first_cursor,
			"volatile spool restart reused cursor space");
	}
	{
		/* A persistent spool must stay monotonic even with a corrupt
		 * lease: its own database still holds the issued maximum. */
		remove_database(path);
		std::uint64_t persisted = 0;
		{
			DurableMeterSpool spool(path,
				spool_policy(StorageBackend::persistent));
			persisted = spool.publish(record(3));
		}
		std::ofstream(path.string() + ".cursor-lease")
			<< "not a number";
		DurableMeterSpool spool(path,
			spool_policy(StorageBackend::persistent));
		require(spool.publish(record(4)) > persisted,
			"corrupt lease regressed a persistent spool's cursors");
	}
	remove_database(path);
}

void publish_enforces_hard_byte_cap()
{
	using namespace mnc::meter_stream;
	const auto path = temporary_database("spool-cap-test");
	remove_database(path);
	{
		/* Room for roughly four 256-byte records at the accounted
		 * 128-byte overhead; no consumer ever acknowledges. */
		DatabaseStoragePolicy policy{DatabaseDataset::raw_record_spool,
			StorageBackend::memory, {std::nullopt, std::uint64_t{1600}}};
		DurableMeterSpool spool(path, policy);
		spool.register_consumer("lagging");
		std::uint64_t newest = 0;
		for (std::uint64_t sequence = 1; sequence <= 10; ++sequence)
			newest = spool.publish(record(sequence));
		const auto status = spool.status();
		require(status.record_count <= 4,
			"hard byte cap did not bound an unacknowledged spool");
		require(status.dropped_unacknowledged_records > 0,
			"unacknowledged evictions were not counted");
		const auto survivors = spool.read_after("lagging", 16);
		require(!survivors.empty() &&
			survivors.back().cursor == newest,
			"the newest record must survive the cap");
		for (std::size_t index = 1; index < survivors.size(); ++index)
			require(survivors[index - 1].cursor < survivors[index].cursor,
				"cap eviction broke cursor ordering");
	}
	remove_database(path);
}

void register_consumer_preserves_acknowledged_cursor()
{
	using namespace mnc::meter_stream;
	const auto path = temporary_database("spool-reregister-test");
	remove_database(path);
	{
		DurableMeterSpool spool(path,
			spool_policy(StorageBackend::memory));
		spool.register_consumer("historian");
		const auto first = spool.publish(record(1));
		(void)spool.publish(record(2));
		spool.acknowledge("historian", first);
		/* The historian re-registers on every consume error; a live
		 * registration must keep its cursor or replay would follow
		 * every transient fault. */
		spool.register_consumer("historian");
		require(spool.read_after("historian", 16).size() == 1,
			"re-registration reset an acknowledged cursor");
	}
	remove_database(path);
}

void backfill_predicate_reports_lost_coverage()
{
	using msap1::history::backfill_is_incomplete;
	/* Virgin historian, empty fresh spool: nothing was ever published. */
	require(!backfill_is_incomplete(0, 1000, 0),
		"an empty fresh spool must count as complete");
	/* Virgin historian, spool holding everything it ever issued. */
	require(!backfill_is_incomplete(1001, 1000, 0),
		"a complete first session was reported incomplete");
	/* Records pruned within the session are lost coverage. */
	require(backfill_is_incomplete(1002, 1000, 0),
		"in-session pruning was not reported");
	/* A spool session that began after durable history existed cannot
	 * rebuild what came before it — the volatile-restart case, including
	 * the empty spool that previously masqueraded as complete. */
	require(backfill_is_incomplete(2000, 1999, 1500),
		"a post-coverage spool session was reported complete");
	require(backfill_is_incomplete(0, 1999, 1500),
		"an empty post-coverage spool was reported complete");
	/* Durable coverage extending past the session start is continuity. */
	require(!backfill_is_incomplete(5, 4, 10),
		"a covering session was reported incomplete");
}

void database_policy_updates_are_idempotent_and_replay_only_when_needed()
{
	using mnc::meter_stream::DatabaseDataset;
	using mnc::meter_stream::DatabaseStoragePolicy;
	using mnc::meter_stream::StorageBackend;
	using mnc::meter_stream::same_database_policies;
	using msap1::history::historian_policy_transition_requires_backfill;

	const std::vector<DatabaseStoragePolicy> current{
		{DatabaseDataset::basic, StorageBackend::memory,
			{std::chrono::hours(24), 1024}},
		{DatabaseDataset::minutes_10, StorageBackend::persistent, {}},
	};
	auto reordered = current;
	std::ranges::reverse(reordered);
	require(same_database_policies(current, reordered),
		"policy equality depends on wire order");
	auto duplicate = reordered;
	duplicate.back() = duplicate.front();
	require(!same_database_policies(current, duplicate),
		"duplicate dataset policies compared equal");
	require(!historian_policy_transition_requires_backfill(
			current, reordered),
		"identical historian policy requested a replay");

	auto retention_only = current;
	retention_only.front().retention.maximum_bytes = 2048;
	require(!same_database_policies(current, retention_only),
		"retention policy change was treated as identical");
	require(!historian_policy_transition_requires_backfill(
			current, retention_only),
		"retention-only policy change requested a replay");

	auto newly_volatile = current;
	newly_volatile.back().backend = StorageBackend::memory;
	require(historian_policy_transition_requires_backfill(
			current, newly_volatile),
		"persistent-to-memory policy change did not request replay");

	auto newly_persistent = current;
	newly_persistent.front().backend = StorageBackend::persistent;
	require(!historian_policy_transition_requires_backfill(
			current, newly_persistent),
		"memory-to-persistent policy change requested a replay");
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

msap1::MeterUpdate energy_demand_history_update()
{
	msap1::MeterUpdate update;
	update.period = msap1::MeasurementPeriod::Min10;
	update.kind = msap1::RecordKind::demand;
	update.sequence = 600;
	update.configuration_generation = 9;
	update.energy.emplace();
	update.demand.emplace();
	update.energy->reset_epoch = 41;
	update.demand->peak_reset_epoch = 42;
	msap1::EnergyCounterArray counters{};
	for (std::size_t index = 0; index < counters.size(); ++index)
		counters[index] = 9'007'199'254'740'993ll +
			static_cast<std::int64_t>(index);
	msap1::assign_energy_counters(*update.energy, counters);
	const auto stamp = [](auto &group, std::uint64_t sequence) {
		for (auto *reading : {&group.phase_a, &group.phase_b,
			&group.phase_c, &group.total}) {
			reading->quality = msap1::MeasurementQuality::valid;
			reading->source_sequence = sequence;
		}
	};
	stamp(update.energy->active_import, 599);
	stamp(update.energy->active_export, 599);
	stamp(update.energy->apparent, 599);
	for (auto &quadrant : update.energy->reactive_quadrants)
		stamp(quadrant, 599);
	msap1::DemandValueArray current{-11, 12, -13, 14};
	msap1::DemandValueArray import{21, 22, 23, 24};
	msap1::DemandValueArray export_values{31, 32, 33, 34};
	msap1::assign_demand_values(update.demand->current_active, current);
	msap1::assign_demand_values(update.demand->import_peak, import);
	msap1::assign_demand_values(update.demand->export_peak, export_values);
	stamp(update.demand->current_active, 600);
	stamp(update.demand->import_peak, 600);
	stamp(update.demand->export_peak, 600);
	return update;
}

void historian_persists_atomic_m17_boundary_snapshots()
{
	using D = mnc::meter_stream::DatabaseDataset;
	using B = mnc::meter_stream::StorageBackend;
	const auto path = temporary_database("history-m17-test");
	remove_database(path);
	const std::vector<mnc::meter_stream::DatabaseStoragePolicy> policies{
		{D::basic, B::memory, {}},
		{D::cycles_150_180, B::persistent, {}},
		{D::minutes_10, B::persistent, {}},
		{D::hours_2, B::persistent, {}},
		{D::harmonic_cycles_150_180, B::memory, {}},
		{D::harmonic_minutes_10, B::persistent, {}},
		{D::harmonic_hours_2, B::persistent, {}},
		{D::demand, B::persistent, {}},
	};
	const msap1::history::HistoryQuery energy_query{
		.period = msap1::MeasurementPeriod::Min10,
		.attributes = {
			mnc::meter::MeterAttributeId::ActiveImportEnergyA,
			mnc::meter::MeterAttributeId::ReactiveEnergyQuadrantIVTotal,
		},
		.start_nanoseconds = 0,
		.end_nanoseconds = 700'000'000'000ll,
		.limit = 64,
		.after = std::nullopt,
	};
	const msap1::history::HistoryQuery demand_query{
		.period = msap1::MeasurementPeriod::Demand,
		.attributes = {
			mnc::meter::MeterAttributeId::CurrentActiveDemandA,
			mnc::meter::MeterAttributeId::ExportDemandPeakTotal,
		},
		.start_nanoseconds = 0,
		.end_nanoseconds = 700'000'000'000ll,
		.limit = 64,
		.after = std::nullopt,
	};
	{
		msap1::history::MeterHistoryStore history(path, policies);
		auto energy_update = energy_demand_history_update();
		energy_update.demand.reset();
		history.append(energy_update, 600, 600'000'000'000ll);
		auto demand_update = energy_demand_history_update();
		demand_update.period = msap1::MeasurementPeriod::Demand;
		demand_update.energy.reset();
		history.append(demand_update, 604, 600'000'000'000ll);
		const auto energy_points = history.query(energy_query);
		const auto demand_points = history.query(demand_query);
		require(energy_points.size() == 2 && demand_points.size() == 2,
			"M17 boundary history did not route energy and demand atomically");
		require(energy_points.front().value == 9'007'199'254'740'993ll,
			"M17 historian narrowed an energy value above JavaScript safe integer");
		for (const auto &point : energy_points)
			require(point.reset_epoch == 41u,
				"M17 historian lost an energy reset epoch");
		for (const auto &point : demand_points)
			require(point.reset_epoch == 42u,
				"M17 historian lost a demand reset epoch");
	}
	{
		msap1::history::MeterHistoryStore reopened(path, policies);
		require(reopened.query(energy_query).size() == 2 &&
			reopened.query(demand_query).size() == 2,
			"M17 energy/demand boundary snapshot was not persistent");
	}
	remove_database(path);
}

void historian_persists_complete_m19_scalar_projection()
{
	using D = mnc::meter_stream::DatabaseDataset;
	using B = mnc::meter_stream::StorageBackend;
	using Id = mnc::meter::MeterAttributeId;
	const auto path = temporary_database("history-m19-scalar-test");
	remove_database(path);
	const std::vector<mnc::meter_stream::DatabaseStoragePolicy> policies{
		{D::basic, B::persistent, {}},
		{D::cycles_150_180, B::persistent, {}},
		{D::minutes_10, B::persistent, {}},
		{D::hours_2, B::persistent, {}},
		{D::harmonic_cycles_150_180, B::memory, {}},
		{D::harmonic_minutes_10, B::persistent, {}},
		{D::harmonic_hours_2, B::persistent, {}},
		{D::demand, B::persistent, {}},
	};
	{
	msap1::history::MeterHistoryStore history(path, policies);
	auto stamp = [](auto &reading, std::int64_t value,
		std::uint64_t sequence) {
		reading.value = value;
		reading.quality = msap1::MeasurementQuality::valid;
		reading.source_sequence = sequence;
	};

	msap1::MeterUpdate power;
	power.period = msap1::MeasurementPeriod::Basic;
	power.kind = msap1::RecordKind::power;
	power.sequence = 10;
	power.power.emplace();
	stamp(power.power->active_power.phase_a, 101, 10);
	stamp(power.power->voltage_crest.phase_a, 14142, 10);
	history.append(power, 10, 1'000'000'000ll);

	msap1::MeterUpdate phasor;
	phasor.period = msap1::MeasurementPeriod::Basic;
	phasor.kind = msap1::RecordKind::phasor;
	phasor.sequence = 11;
	phasor.phasor.emplace();
	stamp(phasor.phasor->fundamental_voltage.phase_a, 120'000'000, 11);
	stamp(phasor.phasor->current_angle.neutral, 359000, 11);
	stamp(phasor.phasor->displacement_power_factor.phase_a, 900000, 11);
	phasor.phasor->load_nature.phase_a = msap1::LoadNature::lagging;
	history.append(phasor, 11, 1'000'000'000ll);

	msap1::MeterUpdate unbalance;
	unbalance.period = msap1::MeasurementPeriod::Basic;
	unbalance.kind = msap1::RecordKind::unbalance;
	unbalance.sequence = 12;
	unbalance.unbalance.emplace();
	stamp(unbalance.unbalance->voltage_positive_sequence, 119'000'000, 12);
	stamp(unbalance.unbalance->voltage_positive_angle, 250, 12);
	history.append(unbalance, 12, 1'000'000'000ll);

	const msap1::history::HistoryQuery query{
		.period = msap1::MeasurementPeriod::Basic,
		.attributes = {Id::ActivePowerA, Id::VoltageCrestA,
			Id::FundamentalVoltageA, Id::CurrentPhaseAngleN,
			Id::LoadNatureA, Id::PositiveSequenceVoltage,
			Id::VoltagePositiveSequenceAngle},
		.start_nanoseconds = 0,
		.end_nanoseconds = 2'000'000'000ll,
		.limit = 64,
		.after = std::nullopt,
	};
	const auto points = history.query(query);
	require(points.size() == query.attributes.size(),
		"M19 historian omitted a decoded scalar family");
	/* Page directly through one timestamp containing multiple record kinds and
	 * attributes. The complete cursor must reproduce the unpaged order without
	 * timestamp overlap, duplicate suppression, or missing siblings. */
	auto paged_query = query;
	paged_query.limit = 2;
	std::vector<msap1::history::HistoryPoint> paged;
	for (;;) {
		const auto page = history.query(paged_query);
		paged.insert(paged.end(), page.begin(), page.end());
		if (page.size() < paged_query.limit)
			break;
		paged_query.after = page.back().cursor;
	}
	require(paged.size() == points.size(),
		"history cursor omitted or duplicated a bounded page sibling");
	for (std::size_t index = 0; index < points.size(); ++index)
		require(paged[index].cursor == points[index].cursor &&
			paged[index].attribute == points[index].attribute &&
			paged[index].value == points[index].value,
			"history cursor did not preserve the complete ordering key");
	const auto nature = std::ranges::find_if(points, [](const auto &point) {
		return point.attribute == Id::LoadNatureA;
	});
	require(nature != points.end() &&
		nature->value == static_cast<std::int64_t>(msap1::LoadNature::lagging),
		"load nature did not retain categorical identity");

	/* The final Min10 sibling and sampled Demand legitimately share one
	 * durable stream cursor but remain independently queryable periods. */
	msap1::MeterUpdate min10;
	min10.period = msap1::MeasurementPeriod::Min10;
	min10.kind = msap1::RecordKind::unbalance;
	min10.sequence = 20;
	min10.unbalance.emplace();
	stamp(min10.unbalance->voltage_unbalance, 1000, 20);
	history.append(min10, 20, 600'000'000'000ll);
	auto demand = energy_demand_history_update();
	demand.period = msap1::MeasurementPeriod::Demand;
	demand.energy.reset();
	history.append(demand, 20, 600'000'000'000ll);
	const msap1::history::HistoryQuery min10_query{
		.period = msap1::MeasurementPeriod::Min10,
		.attributes = {Id::VoltageUnbalance},
		.start_nanoseconds = 0,
		.end_nanoseconds = 700'000'000'000ll,
		.limit = 8,
		.after = std::nullopt,
	};
	const msap1::history::HistoryQuery demand_query{
		.period = msap1::MeasurementPeriod::Demand,
		.attributes = {Id::CurrentActiveDemandA},
		.start_nanoseconds = 0,
		.end_nanoseconds = 700'000'000'000ll,
		.limit = 8,
		.after = std::nullopt,
	};
	require(history.query(min10_query).size() == 1 &&
		history.query(demand_query).size() == 1,
		"shared Min10/Demand stream cursor lost one period projection");
	}
	remove_database(path);
}

void historian_wal_stays_bounded_under_sustained_appends()
{
	using D = mnc::meter_stream::DatabaseDataset;
	using B = mnc::meter_stream::StorageBackend;
	const auto path = temporary_database("history-wal-test");
	remove_database(path);
	{
		const std::vector<mnc::meter_stream::DatabaseStoragePolicy> policies{
			{D::basic, B::persistent, {}},
			{D::cycles_150_180, B::persistent, {}},
			{D::minutes_10, B::persistent, {}},
			{D::hours_2, B::persistent, {}},
			{D::harmonic_cycles_150_180, B::memory, {}},
			{D::harmonic_minutes_10, B::persistent, {}},
			{D::harmonic_hours_2, B::persistent, {}},
			{D::demand, B::persistent, {}},
		};
		msap1::history::MeterHistoryStore history(path, policies);
		/* Enough commits that SQLite's 1000-page auto-checkpoint must fire
		 * many times over.  With any statement left row-active across the
		 * append's COMMIT, every checkpoint aborts, the main database file
		 * never grows past its empty schema, and the WAL accumulates one
		 * commit's frames forever — 1.4 GB in the field.  Bounding both
		 * files here is the regression test for that failure mode. */
		auto update = fundamental_update();
		for (std::uint64_t cursor = 1; cursor <= 2000; ++cursor) {
			update.sequence = cursor;
			history.append(update, cursor,
				2'000'000'000 + static_cast<std::int64_t>(cursor));
		}
		std::error_code error;
		const auto wal_bytes = std::filesystem::file_size(
			std::filesystem::path(path.string() + "-wal"), error);
		require(!error, "historian WAL file is missing");
		require(wal_bytes < 8u * 1024u * 1024u,
			"historian WAL grew unbounded: checkpoints are not completing");
		const auto main_bytes = std::filesystem::file_size(path, error);
		require(!error && main_bytes > 64u * 1024u,
			"checkpoints never backfilled the main database");
	}
	remove_database(path);
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
		{D::harmonic_cycles_150_180, B::memory, {}},
		{D::harmonic_minutes_10, B::persistent, {}},
		{D::harmonic_hours_2, B::persistent, {}},
		{D::demand, B::persistent, {}},
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

void historian_status_reports_incremental_storage_and_indexed_range()
{
	using D = mnc::meter_stream::DatabaseDataset;
	using B = mnc::meter_stream::StorageBackend;
	const auto path = temporary_database("history-status-test");
	remove_database(path);
	const std::vector<mnc::meter_stream::DatabaseStoragePolicy> policies{
		{D::basic, B::persistent, {}},
		{D::cycles_150_180, B::persistent, {}},
		{D::minutes_10, B::persistent, {}},
		{D::hours_2, B::persistent, {}},
		{D::harmonic_cycles_150_180, B::memory, {}},
		{D::harmonic_minutes_10, B::persistent, {}},
		{D::harmonic_hours_2, B::persistent, {}},
		{D::demand, B::persistent, {}},
	};
	auto verify = [](const msap1::history::HistorianStatus &status) {
		const auto basic = std::ranges::find_if(status.datasets,
			[](const auto &item) { return item.dataset == D::basic; });
		require(basic != status.datasets.end(),
			"basic dataset is missing from historian status");
		require(basic->block_count == 3,
			"historian status block count drifted from committed rows");
		require(basic->storage_bytes == 3u * (96u + 11u * 40u),
			"historian status did not use exact incremental storage bytes");
		require(basic->oldest_nanoseconds == 10'000'000'000ll &&
			basic->newest_nanoseconds == 30'000'000'000ll,
			"historian status did not report exact indexed time bounds");
	};
	{
		msap1::history::MeterHistoryStore history(path, policies);
		for (const auto &[cursor, measured_at] :
		     std::array<std::pair<std::uint64_t, std::int64_t>, 3>{
			     {{1, 30'000'000'000ll}, {2, 10'000'000'000ll},
			      {3, 20'000'000'000ll}}}) {
			auto update = fundamental_update();
			update.sequence = cursor;
			history.append(update, cursor, measured_at);
		}
		verify(history.status());
		verify(history.status());
	}
	{
		/* A restart drops only the in-process counter. Its one-time seed must
		 * recover the same exact total; later status polls reuse that value. */
		msap1::history::MeterHistoryStore history(path, policies);
		verify(history.status());
		verify(history.status());
	}
	remove_database(path);
}

void historian_commits_harmonics_as_one_durable_family()
{
	using D = mnc::meter_stream::DatabaseDataset;
	using B = mnc::meter_stream::StorageBackend;
	const auto path = temporary_database("harmonic-history-test");
	remove_database(path);
	const std::vector<mnc::meter_stream::DatabaseStoragePolicy> policies{
		{D::basic, B::memory, {}},
		{D::cycles_150_180, B::persistent, {}},
		{D::minutes_10, B::persistent, {}},
		{D::hours_2, B::persistent, {}},
		{D::harmonic_cycles_150_180, B::persistent, {}},
		{D::harmonic_minutes_10, B::persistent, {}},
		{D::harmonic_hours_2, B::persistent, {}},
		{D::demand, B::persistent, {}},
	};
	auto family_count = [](const msap1::history::HistorianStatus &status) {
		const auto dataset = std::ranges::find_if(status.datasets,
			[](const auto &item) {
				return item.dataset == D::harmonic_cycles_150_180;
			});
		return dataset == status.datasets.end() ? 0u : dataset->block_count;
	};
	{
		msap1::history::MeterHistoryStore history(path, policies);
		std::uint64_t cursor = 0;
		for (std::uint8_t channel = 0;
		     channel < msap1::harmonic_channel_count; ++channel) {
			for (std::uint8_t chunk = 0;
			     chunk < msap1::harmonic_chunks_per_channel; ++chunk) {
				++cursor;
				const auto completed = history.append_harmonic_record(
					harmonic_aggregate_record(3, channel, chunk), cursor,
					2'000'000'000 + static_cast<std::int64_t>(cursor));
				require(completed ==
					(cursor == msap1::harmonic_records_per_family),
					"harmonic family became visible before its final chunk");
			}
		}
		require(family_count(history.status()) == 1u,
			"completed harmonic family was not materialized exactly once");
		require(history.persisted_stream_high_water() ==
			msap1::harmonic_records_per_family,
			"harmonic staging cursor was not made durable");
	}
	{
		msap1::history::MeterHistoryStore history(path, policies);
		require(family_count(history.status()) == 1u,
			"harmonic family did not survive historian restart");
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

	/* One block is 96 bytes plus 40 per reading, and every M19 fundamental
	 * append writes all eleven scalar attributes: 536 bytes. A 1200-byte cap
	 * therefore holds exactly two
	 * blocks, so from the third append onward the oldest must be evicted and
	 * exactly two must remain. A cap that evicted the whole selection batch
	 * instead of just the surplus would leave zero. */
	{
		const std::vector<mnc::meter_stream::DatabaseStoragePolicy> policies{
			{D::basic, B::memory, {std::nullopt, 1200ull}},
			{D::cycles_150_180, B::persistent, {}},
			{D::minutes_10, B::persistent, {}},
			{D::hours_2, B::persistent, {}},
			{D::harmonic_cycles_150_180, B::memory, {}},
			{D::harmonic_minutes_10, B::persistent, {}},
			{D::harmonic_hours_2, B::persistent, {}},
			{D::demand, B::persistent, {}},
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
			{D::harmonic_cycles_150_180, B::memory, {}},
			{D::harmonic_minutes_10, B::persistent, {}},
			{D::harmonic_hours_2, B::persistent, {}},
			{D::demand, B::persistent, {}},
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
			{D::harmonic_cycles_150_180, B::memory, {}},
			{D::harmonic_minutes_10, B::persistent, {}},
			{D::harmonic_hours_2, B::persistent, {}},
			{D::demand, B::persistent, {}},
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
			{D::harmonic_cycles_150_180, B::memory, {}},
			{D::harmonic_minutes_10, B::persistent, {}},
			{D::harmonic_hours_2, B::persistent, {}},
			{D::demand, B::persistent, {}},
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
		{D::harmonic_cycles_150_180, B::memory, {}},
		{D::harmonic_minutes_10, B::persistent, {}},
		{D::harmonic_hours_2, B::persistent, {}},
		{D::demand, B::persistent, {}},
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
			.after = std::nullopt,
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
			.after = std::nullopt,
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
	spool_distinguishes_source_fragments();
	spool_atomically_publishes_record_families();
	spool_age_pruning_is_indexed_and_preserves_fresh_records();
	spool_migrates_legacy_identity_key();
	spool_backend_switch_replaces_stale_target();
	memory_spool_cursors_survive_restart();
	publish_enforces_hard_byte_cap();
	register_consumer_preserves_acknowledged_cursor();
	backfill_predicate_reports_lost_coverage();
	database_policy_updates_are_idempotent_and_replay_only_when_needed();
	historian_wal_stays_bounded_under_sustained_appends();
	malformed_policies_are_rejected();
	historian_preserves_quality_and_storage_routing();
	historian_persists_atomic_m17_boundary_snapshots();
	historian_persists_complete_m19_scalar_projection();
	historian_status_reports_incremental_storage_and_indexed_range();
	historian_commits_harmonics_as_one_durable_family();
	historian_enforces_retention_without_rescanning();
	historian_maintenance_preserves_explicit_clear_boundary();
}
