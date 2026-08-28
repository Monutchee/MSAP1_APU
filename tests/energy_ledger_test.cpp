#include "msap1/meter/energy_ledger.hpp"
#include "mnc/storage/sqlite/sqlite_database.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

#include <unistd.h>

namespace {

int failures = 0;

void require(bool condition, const char *message)
{
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		++failures;
	}
}

msap1::EnergyValues energy(std::uint64_t session_id, std::int64_t counter)
{
	msap1::EnergyValues result;
	msap1::EnergyCounterArray counters{};
	counters.fill(counter);
	msap1::assign_energy_counters(result, counters);
	result.session_id = session_id;
	result.accepted_samples = static_cast<std::uint64_t>(counter) * 100u;
	result.accepted_blocks = static_cast<std::uint32_t>(counter);
	const auto stamp = [&](auto &group) {
		for (auto *reading : {&group.phase_a, &group.phase_b,
			&group.phase_c, &group.total}) {
			reading->quality = msap1::MeasurementQuality::valid;
			reading->calculation_window = {1000u,
				std::chrono::seconds(1)};
		}
	};
	stamp(result.active_import);
	stamp(result.active_export);
	stamp(result.apparent);
	for (auto &quadrant : result.reactive_quadrants)
		stamp(quadrant);
	return result;
}

msap1::DemandValues demand(std::uint64_t session_id, std::int64_t peak)
{
	msap1::DemandValues result;
	result.session_id = session_id;
	result.interval_target_sample = 600000u + static_cast<std::uint64_t>(peak);
	result.time_aligned = true;
	result.boundary_valid = true;
	msap1::DemandValueArray current{1, -2, 3, -4};
	msap1::DemandValueArray peaks{};
	peaks.fill(peak);
	msap1::assign_demand_values(result.current_active, current);
	msap1::assign_demand_values(result.import_peak, peaks);
	msap1::assign_demand_values(result.export_peak, peaks);
	result.import_peak_sample = {1u, 2u, 3u, 4u};
	result.export_peak_sample = {5u, 6u, 7u, 8u};
	const auto stamp = [](auto &group) {
		for (auto *reading : {&group.phase_a, &group.phase_b,
			&group.phase_c, &group.total}) {
			reading->quality = msap1::MeasurementQuality::valid;
			reading->calculation_window = {19200000u,
				std::chrono::seconds(600)};
		}
	};
	stamp(result.current_active);
	stamp(result.import_peak);
	stamp(result.export_peak);
	return result;
}

msap1::energy_ledger::ResetRequest reset_request(std::uint64_t epoch,
	std::string key)
{
	return {.expected_epoch = epoch, .idempotency_key = std::move(key),
		.actor = "admin", .request_id = "request-1",
		.requested_at_nanoseconds = 123456789};
}

template<typename Exception, typename Function>
void throws(Function function, const char *message)
{
	try {
		function();
	} catch (const Exception &) {
		return;
	}
	require(false, message);
}

void remove_database(const std::filesystem::path &path)
{
	std::filesystem::remove(path);
	std::filesystem::remove(path.string() + "-wal");
	std::filesystem::remove(path.string() + "-shm");
}

void execute_sql(const std::filesystem::path &path, std::string_view sql)
{
	mnc::storage::sqlite::Database database(path);
	database.execute(sql);
}

std::int64_t scalar(const std::filesystem::path &path, std::string_view sql)
{
	mnc::storage::sqlite::Database database(path);
	auto query = database.prepare(sql);
	require(query.step(), "SQLite scalar query returned no row");
	return query.integer(0);
}

void test_ledger()
{
	const auto base = std::filesystem::temp_directory_path() /
		("msap1-energy-ledger-test-" + std::to_string(::getpid()) + ".sqlite3");
	remove_database(base);
	{
		msap1::energy_ledger::EnergyLedger ledger(base);
		throws<msap1::energy_ledger::Unavailable>(
			[&] { (void)ledger.reset_energy(reset_request(0, "no-checkpoint")); },
			"energy reset without checkpoint must be unavailable");

		auto first = ledger.ingest_energy(energy(0x10u, 10), 1u, 1u, 100);
		require(first.active_import.phase_a.value == 10,
			"first session checkpoint initializes lifetime counters");
		auto same = ledger.ingest_energy(energy(0x10u, 15), 2u, 1u, 200);
		require(same.active_import.phase_a.value == 15,
			"same session adds only monotonic delta");
		auto duplicate = ledger.ingest_energy(energy(0x10u, 15), 2u, 1u, 200);
		require(duplicate.active_import.phase_a.value == 15,
			"duplicate ENERGY family is idempotent");
		throws<msap1::energy_ledger::Conflict>(
			[&] { (void)ledger.ingest_energy(energy(0x10u, 14), 3u, 1u, 300); },
			"same-session counter rollback must be rejected");
		auto restarted = ledger.ingest_energy(energy(0x20u, 3), 1u, 1u, 400);
		require(restarted.active_import.phase_a.value == 18,
			"new R5C1 session accumulates from zero without losing lifetime");

		const auto reset = ledger.reset_energy(reset_request(0, "energy-reset"));
		require(reset.epoch == 1u && !reset.replayed &&
			ledger.energy()->active_import.phase_a.value == 0,
			"energy reset zeros all lifetime counters and advances epoch");
		const auto replay = ledger.reset_energy(reset_request(0, "energy-reset"));
		require(replay.epoch == 1u && replay.replayed,
			"energy reset idempotency key replays original result");
		throws<msap1::energy_ledger::Conflict>(
			[&] { (void)ledger.reset_energy(reset_request(0, "wrong-epoch")); },
			"energy reset expected epoch conflict");
		auto after_reset = ledger.ingest_energy(energy(0x20u, 5), 2u, 1u, 500);
		require(after_reset.active_import.phase_a.value == 2 &&
			after_reset.reset_epoch == 1u,
			"post-reset same-session accumulation starts at durable baseline");

		auto first_demand = ledger.ingest_demand(demand(0x20u, 100), 1u, 1u, 600);
		require(first_demand.import_peak.phase_a.value == 100,
			"first demand session peak becomes authoritative");
		const auto peak_reset = ledger.reset_demand_peaks(
			reset_request(0, "demand-reset"));
		require(peak_reset.epoch == 1u &&
			ledger.demand()->import_peak.phase_a.value == 0,
			"demand reset zeros authoritative directional peaks");
		auto replayed_interval = ledger.ingest_demand(
			demand(0x20u, 100), 1u, 1u, 650);
		require(replayed_interval.import_peak.phase_a.value == 0 &&
			replayed_interval.export_peak.phase_b.value == 0 &&
			replayed_interval.peak_reset_epoch == 1u,
			"pre-reset DEMAND replay crossed the reset watermark");
		auto contaminated = demand(0x20u, 100);
		contaminated.contaminated = true;
		for (auto *reading : {&contaminated.current_active.phase_a,
			&contaminated.current_active.phase_b,
			&contaminated.current_active.phase_c,
			&contaminated.current_active.total})
			reading->quality = msap1::MeasurementQuality::invalid;
		auto rejected_interval = ledger.ingest_demand(
			contaminated, 2u, 1u, 700);
		require(rejected_interval.import_peak.phase_a.value == 0 &&
			rejected_interval.export_peak.phase_b.value == 0,
			"invalid post-reset interval updated a demand peak");
		auto post_reset = ledger.ingest_demand(
			demand(0x20u, 100), 3u, 1u, 750);
		require(post_reset.import_peak.phase_a.value == 1 &&
			post_reset.export_peak.phase_b.value == 2,
			"valid post-reset interval establishes the new epoch peak without "
			"resurfacing the old RPU peak");
		auto new_peak = ledger.ingest_demand(demand(0x20u, 120), 4u, 1u, 800);
		require(new_peak.import_peak.phase_a.value == 120 &&
			new_peak.peak_reset_epoch == 1u,
			"post-reset RPU peak increase becomes authoritative at full value");
	}
	{
		msap1::energy_ledger::EnergyLedger reopened(base);
		require(reopened.energy() &&
			reopened.energy()->active_import.phase_a.value == 2 &&
			reopened.energy()->reset_epoch == 1u,
			"energy lifetime and epoch recover after process restart");
		require(reopened.demand() &&
			reopened.demand()->import_peak.phase_a.value == 120 &&
			reopened.demand()->peak_reset_epoch == 1u,
			"demand peaks and epoch recover after process restart");
	}
	remove_database(base);
}

void test_failed_commit_retry_and_session_history()
{
	const auto base = std::filesystem::temp_directory_path() /
		("msap1-energy-ledger-failure-test-" +
		 std::to_string(::getpid()) + ".sqlite3");
	remove_database(base);
	{
		msap1::energy_ledger::EnergyLedger ledger(base);
		(void)ledger.ingest_energy(energy(0x31u, 10), 1u, 1u, 100);
		execute_sql(base,
			"CREATE TRIGGER fail_energy_state BEFORE INSERT ON energy_state "
			"BEGIN SELECT RAISE(ABORT,'forced energy failure'); END");
		throws<std::runtime_error>(
			[&] {
				(void)ledger.ingest_energy(energy(0x31u, 15), 2u, 1u, 200);
			},
			"forced ENERGY commit failure did not propagate");
		require(ledger.energy()->active_import.phase_a.value == 10,
			"failed ENERGY commit advanced the in-memory checkpoint");
		execute_sql(base, "DROP TRIGGER fail_energy_state");
		auto energy_retry =
			ledger.ingest_energy(energy(0x31u, 15), 2u, 1u, 200);
		require(energy_retry.active_import.phase_a.value == 15,
			"ENERGY retry after a failed commit was not accepted");

		auto first_demand = demand(0x31u, 100);
		first_demand.interval_target_sample = 700000u;
		(void)ledger.ingest_demand(first_demand, 1u, 1u, 300);
		execute_sql(base,
			"CREATE TRIGGER fail_demand_state BEFORE INSERT ON demand_state "
			"BEGIN SELECT RAISE(ABORT,'forced demand failure'); END");
		auto second_demand = demand(0x31u, 120);
		second_demand.interval_target_sample = 800000u;
		throws<std::runtime_error>(
			[&] { (void)ledger.ingest_demand(second_demand, 2u, 1u, 400); },
			"forced DEMAND commit failure did not propagate");
		require(ledger.demand()->import_peak.phase_a.value == 100,
			"failed DEMAND commit advanced the in-memory checkpoint");
		execute_sql(base, "DROP TRIGGER fail_demand_state");
		auto demand_retry = ledger.ingest_demand(second_demand, 2u, 1u, 400);
		require(demand_retry.import_peak.phase_a.value == 120,
			"DEMAND retry after a failed commit was not accepted");

		/* A full reboot can restart the conversion-domain sample index. The
		 * boot session ID keeps equal target anchors as distinct snapshots. */
		(void)ledger.ingest_energy(energy(0x32u, 1), 1u, 1u, 500);
		auto reboot_demand = demand(0x32u, 1);
		reboot_demand.interval_target_sample = 700000u;
		(void)ledger.ingest_demand(reboot_demand, 1u, 1u, 600);
		require(scalar(base, "SELECT COUNT(*) FROM energy_history") == 3,
			"session-scoped ENERGY history lost a reboot snapshot");
		require(scalar(base, "SELECT COUNT(*) FROM demand_history") == 3,
			"session-scoped DEMAND history lost a reboot snapshot");
	}
	remove_database(base);
}

} // namespace

int main()
{
	test_ledger();
	test_failed_commit_retry_and_session_history();
	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
