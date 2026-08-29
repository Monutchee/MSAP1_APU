#include "msap1/meter/history/meter_history.hpp"
#include "msap1/meter/meter_record.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <vector>

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

std::filesystem::path database_path()
{
	return std::filesystem::temp_directory_path() /
		("msap1-pq-event-history-" + std::to_string(::getpid()) + ".sqlite3");
}

void remove_database(const std::filesystem::path &path)
{
	std::error_code ignored;
	std::filesystem::remove(path, ignored);
	std::filesystem::remove(path.string() + "-wal", ignored);
	std::filesystem::remove(path.string() + "-shm", ignored);
}

std::vector<mnc::meter_stream::DatabaseStoragePolicy> policies()
{
	using D = mnc::meter_stream::DatabaseDataset;
	using B = mnc::meter_stream::StorageBackend;
	return {
		{D::basic, B::memory, {}},
		{D::cycles_150_180, B::persistent, {}},
		{D::minutes_10, B::persistent, {}},
		{D::hours_2, B::persistent, {}},
		{D::harmonic_cycles_150_180, B::memory, {}},
		{D::harmonic_minutes_10, B::persistent, {}},
		{D::harmonic_hours_2, B::persistent, {}},
		{D::demand, B::persistent, {}},
	};
}

msap1::MeterRecord event_record(std::uint8_t lifecycle,
	std::uint32_t sequence, std::uint64_t last_sample)
{
	constexpr std::uint64_t first_sample = 1000;
	msap1::MeterRecord record{};
	record.words[0] = msap1::meter_record_magic;
	record.words[1] = msap1::meter_pq_event_lifecycle_format;
	record.words[2] = msap1::meter_record_size;
	record.words[3] = sequence;
	record.words[4] = 7;
	record.words[5] = 1000;
	const auto count = last_sample - first_sample + 1u;
	record.words[6] = static_cast<std::uint32_t>(count);
	record.words[7] = 0x10;
	record.words[8] = 0x0a;
	record.words[9] = static_cast<std::uint32_t>(first_sample);
	record.words[10] = static_cast<std::uint32_t>(first_sample >> 32u);
	record.words[13] = lifecycle | (1u << 8u);
	record.words[14] = static_cast<std::uint32_t>(last_sample);
	record.words[15] = static_cast<std::uint32_t>(last_sample >> 32u);
	record.words[16] = 0x05060708;
	record.words[17] = 0x01020304;
	record.words[18] = 9;
	record.words[20] = 7;
	record.words[21] = 9000;
	record.words[22] = 200;
	record.words[23] = 0x5u | (1u << 8u);
	record.words[24] = 100;
	record.words[25] = 500;
	record.words[26] = 230000000;
	for (std::size_t phase = 0; phase < 3; ++phase) {
		record.words[28 + phase] = 180000000 + phase;
		record.words[31 + phase] = 230000000 + phase;
		record.words[34 + phase] = 200000000 + phase;
	}
	const auto duration = last_sample - first_sample;
	record.words[37] = static_cast<std::uint32_t>(duration);
	record.words[38] = static_cast<std::uint32_t>(duration >> 32u);
	record.words[39] = static_cast<std::uint32_t>(first_sample);
	record.words[40] = static_cast<std::uint32_t>(first_sample >> 32u);
	record.words[47] = sequence;
	record.words[48] = 1;
	record.words[49] = 2;
	record.words[50] = 3;
	record.words[51] = 4;
	return record;
}

} // namespace

int main()
{
	const auto path = database_path();
	remove_database(path);
	const msap1::PowerQualityEventId id{0x0102030405060708ull, 9};
	msap1::history::PowerQualityEventQuery by_id{};
	by_id.id = id;
	msap1::history::WaveformCaptureUuid capture{};
	for (std::size_t index = 0; index < capture.size(); ++index)
		capture[index] = static_cast<std::byte>(index + 1u);

	{
		msap1::history::MeterHistoryStore history(path, policies());
		auto start = event_record(msap1::meter_event_lifecycle_start, 1, 1000);
		history.upsert_power_quality_event(start, 10, 1'000'000'000ll,
			msap1::TimeQuality::Synchronized, 250);
		/* At-least-once replay is idempotent. */
		history.upsert_power_quality_event(start, 10, 1'000'000'000ll,
			msap1::TimeQuality::Synchronized, 250);
		auto update = event_record(msap1::meter_event_lifecycle_update, 2, 2000);
		history.upsert_power_quality_event(update, 11, 1'000'000'000ll,
			msap1::TimeQuality::Synchronized, 250);
		/* A late START cannot regress the materialized lifecycle. */
		history.upsert_power_quality_event(start, 12, 1'000'000'000ll,
			msap1::TimeQuality::Synchronized, 250);

		auto rows = history.query_power_quality_events(by_id);
		require(rows.size() == 1, "one stable event is materialized");
		require(rows[0].event.lifecycle ==
				msap1::PowerQualityEventLifecycle::update &&
				rows[0].event.last_sample == 2000 &&
				rows[0].stream_cursor == 11,
			"stale replay cannot regress lifecycle/sample/cursor");
		require(rows[0].start_utc_nanoseconds == 1'000'000'000ll &&
				rows[0].last_utc_nanoseconds == 2'000'000'000ll &&
				rows[0].utc_uncertainty_nanoseconds == 250,
			"UTC range is derived from the sample anchors");

		history.link_power_quality_event_waveform(id, capture);
		history.link_power_quality_event_waveform(id, capture);
		rows = history.query_power_quality_events(by_id);
		require(rows[0].waveform_capture_uuids.size() == 1 &&
				rows[0].waveform_capture_uuids[0] == capture,
			"waveform links are idempotent");
		require(history.status().power_quality_event_count == 1,
			"historian status exposes the event count");
	}
	{
		msap1::history::MeterHistoryStore reopened(path, policies());
		const auto rows = reopened.query_power_quality_events(by_id);
		require(rows.size() == 1 &&
				rows[0].waveform_capture_uuids.size() == 1,
			"event catalogue and links survive restart");
	}
	remove_database(path);
	if (failures != 0) {
		std::fprintf(stderr, "FAILED: %d check(s)\n", failures);
		return EXIT_FAILURE;
	}
	std::printf("PASS: pq_event_history_test\n");
	return EXIT_SUCCESS;
}
