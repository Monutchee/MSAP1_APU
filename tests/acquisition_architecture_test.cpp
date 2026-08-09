#include "msap1/acquisition/dma/meter_record_source.hpp"
#include "msap1/acquisition/rpu/rpu_control.hpp"
#include "msap1/acquisition/ipc/acquisition_ipc.hpp"
#include "pipeline/record_ingestor.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

#include <unistd.h>

namespace {

void require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

class FakeMeterSource final : public msap1::acquisition::MeterRecordSource {
public:
	void start() override { started = true; }
	void stop() noexcept override { started = false; }
	int native_handle() const noexcept override { return 17; }
	std::string_view name() const noexcept override { return "fake-meter"; }
	msap1::acquisition::MeterRecordBatch read_available() override
	{
		msap1::acquisition::MeterRecordBatch batch;
		batch.count = 1;
		batch.bytes = sizeof(msap1::MeterRecord);
		return batch;
	}
	bool started = false;
};

class FakeRpuControl final : public msap1::acquisition::RpuControl {
public:
	msap1::Message transact(
		std::uint8_t type, const void *, std::size_t,
		std::chrono::milliseconds) override
	{
		msap1::Message result;
		result.header.type = type;
		return result;
	}

	msap1_adc_health_payload query_health() override
	{
		msap1_adc_health_payload health{};
		health.sample_rate_hz = 32000;
		return health;
	}
};

void device_interfaces_are_substitutable()
{
	FakeMeterSource meter;
	msap1::acquisition::MeterRecordSource &source = meter;
	source.start();
	require(meter.started, "meter source did not start");
	require(source.native_handle() == 17, "wrong fake descriptor");
	require(source.read_available().count == 1, "wrong fake batch");
	source.stop();
	require(!meter.started, "meter source did not stop");

	FakeRpuControl fake;
	msap1::acquisition::RpuControl &rpu = fake;
	require(rpu.transact(9).header.type == 9, "wrong fake RPU response");
	require(rpu.query_health().sample_rate_hz == 32000,
		"wrong fake RPU health");
}

void typed_commands_round_trip_through_the_registry()
{
	msap1::AcquisitionCommandRegistry registry;
	registry.on<msap1::SampleRateSetRequest>(
		msap1::AcquisitionStatus::configuration_error,
		[](const msap1::SampleRateSetRequest &request) {
			require(request.sample_rate_hz == 64000,
				"wrong decoded sample rate");
			msap1::InfoResponse response{};
			response.running = true;
			response.sample_rate_hz = request.sample_rate_hz;
			msap1_adc_health_payload health{};
			health.drdy_frequency_hz = 63999;
			response.rpu_health = health;
			return response;
		});

	msap1::SampleRateSetRequest request;
	request.sample_rate_hz = 64000;
	auto frame = msap1::encode_acquisition_request(request);
	frame.correlation_id = 0x1122334455667788ull;
	require(frame.message_type ==
			msap1::acquisition_command_id<msap1::SampleRateSetRequest>,
		"wrong request message type");

	const auto reply = registry.dispatch(frame);
	require(reply.kind == mnc::ipc::FrameKind::response,
		"wrong reply frame kind");
	require(reply.message_type == frame.message_type,
		"reply must echo the command identity");
	require(reply.correlation_id == frame.correlation_id,
		"reply must echo the correlation ID");
	const auto response =
		msap1::decode_acquisition_payload<msap1::InfoResponse>(reply);
	require(response.status == msap1::AcquisitionStatus::ok, "wrong status");
	require(response.running, "wrong running flag");
	require(response.sample_rate_hz == 64000, "wrong response sample rate");
	require(response.rpu_health.value().drdy_frequency_hz == 63999,
		"wrong hardware-mirror payload round trip");
}

/* Record source double that hands the ingestor a scripted batch. */
class ScriptedMeterSource final : public msap1::acquisition::MeterRecordSource {
public:
	void start() override {}
	void stop() noexcept override {}
	int native_handle() const noexcept override { return -1; }
	std::string_view name() const noexcept override { return "scripted"; }
	msap1::acquisition::MeterRecordBatch read_available() override
	{
		auto batch = next;
		next = {};
		return batch;
	}
	msap1::acquisition::MeterRecordBatch next;
};

/* Minimal valid v2 record for the continuity checks; channel words stay
 * zero because validation never inspects the electrical payload. */
msap1::MeterRecord v2_record(std::uint32_t sequence,
			     std::uint64_t first_sample_index,
			     std::uint32_t sample_count,
			     std::uint32_t generation)
{
	msap1::MeterRecord record{};
	record.words[0] = msap1::meter_record_magic;
	record.words[1] = msap1::meter_periodic_format_v2;
	record.words[2] = msap1::meter_record_size;
	record.words[3] = sequence;
	record.words[4] = generation;
	record.words[5] = 32000;
	record.words[6] = sample_count;
	record.words[15] = 60u | (12u << 8) | (1u << 16);
	record.words[60] = static_cast<std::uint32_t>(first_sample_index);
	record.words[61] = static_cast<std::uint32_t>(first_sample_index >> 32);
	return record;
}

void ingestor_validates_v2_sample_range_continuity()
{
	using msap1::acquisition::daemon::MeterRecordIngestor;
	const auto database = std::filesystem::temp_directory_path() /
		("msap1-ingestor-test-" + std::to_string(::getpid()) +
		 ".sqlite3");
	std::filesystem::remove(database);
	{
		ScriptedMeterSource source;
		msap1::MeterRecordStream stream(database);
		msap1::PreparedMeterConfiguration configuration{};
		configuration.wire.generation = 0xfeedbeefu;
		configuration.wire.sample_rate_hz = 32000;
		configuration.wire.rms_window_samples = 6400;
		const msap1::meter::MeasurementTimebase timebase;
		MeterRecordIngestor ingest(source, stream, configuration,
					   timebase);
		ingest.begin_epoch();

		const auto feed = [&](const msap1::MeterRecord &record) {
			source.next = {};
			source.next.records[0] = record;
			source.next.count = 1;
			source.next.bytes = sizeof(msap1::MeterRecord);
			ingest.read_available();
		};

		/* Gapless cycle blocks of varying length are accepted: the
		 * v1 window-equality check must not apply to v2 records. */
		feed(v2_record(1, 640'000, 6400, 0xfeedbeefu));
		feed(v2_record(2, 646'400, 6421, 0xfeedbeefu));
		feed(v2_record(3, 652'821, 6379, 0xfeedbeefu));
		require(ingest.meter_records() == 3 &&
			ingest.sequence_gaps() == 0 &&
			ingest.invalid_records() == 0,
			"gapless variable-length v2 blocks were not accepted");

		/* A first-sample discontinuity with continuous sequences is
		 * lost data: counted as a gap, then resynced. */
		feed(v2_record(4, 700'000, 6400, 0xfeedbeefu));
		require(ingest.meter_records() == 4 &&
			ingest.sequence_gaps() == 1,
			"a first-sample-index gap was not detected");
		feed(v2_record(5, 706'400, 6400, 0xfeedbeefu));
		require(ingest.sequence_gaps() == 1,
			"continuity did not resync after a sample-range gap");

		/* A wrong generation is invalid, exactly as for v1. */
		feed(v2_record(6, 712'800, 6400, 0x12345678u));
		require(ingest.invalid_records() == 1,
			"a stale-generation v2 record was accepted");
	}
	std::filesystem::remove(database);
}

/* Minimal valid MTR2 aggregate for the interleaving checks: 15 blocks of
 * 6400 samples at 32 kSPS, 60 Hz nominal -> 180 cycles. Channel words stay
 * zero because continuity validation never inspects the electrical
 * payload. */
msap1::MeterRecord aggregate_record(std::uint32_t sequence,
				    std::uint64_t first_sample_index,
				    std::uint32_t generation)
{
	msap1::MeterRecord record{};
	record.words[0] = msap1::meter_record_magic;
	record.words[1] = msap1::meter_aggregate_format;
	record.words[2] = msap1::meter_record_size;
	record.words[3] = sequence;
	record.words[4] = generation;
	record.words[5] = 32000;
	record.words[6] = 96'000;
	record.words[7] = 0x7f;
	record.words[8] = (1u << 1) | (1u << 2);
	record.words[9] = sequence * 15u - 14u;
	record.words[10] = sequence * 15u;
	record.words[11] = 15u | (60u << 8) | (180u << 16);
	record.words[12] = static_cast<std::uint32_t>(first_sample_index);
	record.words[13] = static_cast<std::uint32_t>(first_sample_index >> 32);
	record.words[32] = 60'000;
	return record;
}

/*
 * The DMA stream interleaves basic MTR1 and aggregate MTR2 records with
 * INDEPENDENT sequence counters. Continuity must be tracked per format,
 * latest_record() must remain the newest BASIC record, and aggregate
 * timing must be UTC-stamped from the timebase exactly like basic timing.
 */
void ingestor_tracks_interleaved_aggregate_stream()
{
	using msap1::acquisition::daemon::MeterRecordIngestor;
	const auto database = std::filesystem::temp_directory_path() /
		("msap1-ingestor-aggregate-test-" +
		 std::to_string(::getpid()) + ".sqlite3");
	std::filesystem::remove(database);
	{
		ScriptedMeterSource source;
		msap1::MeterRecordStream stream(database);
		msap1::PreparedMeterConfiguration configuration{};
		configuration.wire.generation = 0xfeedbeefu;
		configuration.wire.sample_rate_hz = 32000;
		configuration.wire.rms_window_samples = 6400;
		msap1::meter::MeasurementTimebase timebase;
		/* Trusted sync against the ACTIVE generation: sample 640'000
		 * corresponds exactly to utc_ns below. */
		timebase.record_sync(
			{.sample_counter = 640'000,
			 .utc_ns = 1'700'000'000'000'000'000ll,
			 .uncertainty_ns = 250,
			 .sample_rate_hz = 32000,
			 .configuration_generation = 0xfeedbeefu,
			 .utc_synchronized = true},
			std::chrono::steady_clock::now());
		MeterRecordIngestor ingest(source, stream, configuration,
					   timebase);
		ingest.begin_epoch();

		const auto feed = [&](const msap1::MeterRecord &record) {
			source.next = {};
			source.next.records[0] = record;
			source.next.count = 1;
			source.next.bytes = sizeof(msap1::MeterRecord);
			ingest.read_available();
		};

		/* Aggregates interleave mid-stream without disturbing the
		 * basic sequence/sample-range chain, and vice versa. */
		feed(v2_record(1, 640'000, 6400, 0xfeedbeefu));
		feed(v2_record(2, 646'400, 6400, 0xfeedbeefu));
		feed(aggregate_record(1, 640'000, 0xfeedbeefu));
		feed(v2_record(3, 652'800, 6400, 0xfeedbeefu));
		feed(aggregate_record(2, 640'000, 0xfeedbeefu));
		require(ingest.meter_records() == 5 &&
			ingest.sequence_gaps() == 0 &&
			ingest.aggregate_sequence_gaps() == 0 &&
			ingest.invalid_records() == 0,
			"interleaved records were mistaken for gaps");

		/* A missing aggregate is a gap on the AGGREGATE counter only;
		 * the basic counters must not move. */
		feed(aggregate_record(4, 640'000, 0xfeedbeefu));
		feed(v2_record(4, 659'200, 6400, 0xfeedbeefu));
		require(ingest.aggregate_sequence_gaps() == 1 &&
			ingest.sequence_gaps() == 0 &&
			ingest.meter_records() == 7,
			"an aggregate sequence gap was not isolated per format");

		/* The raw readings cache must remain the newest BASIC record
		 * even though an aggregate arrived after it. */
		require(ingest.latest_record().has_value() &&
			ingest.latest_record()->record_format() ==
				msap1::meter_periodic_format_v2 &&
			ingest.latest_record()->sequence() == 4,
			"latest_record() did not stay on the basic stream");

		/* The aggregate cache is the newest AGGREGATE record and is
		 * not disturbed by the basic record that arrived after it. */
		require(ingest.latest_aggregate_record().has_value() &&
			ingest.latest_aggregate_record()->record_format() ==
				msap1::meter_aggregate_format &&
			ingest.latest_aggregate_record()
					->aggregate_sequence() == 4,
			"latest_aggregate_record() did not cache the newest aggregate");
		require(ingest.aggregate_record_age_ms() !=
				msap1::acquisition_age_unavailable,
			"aggregate freshness was not tracked");

		/* Aggregate publication goes through the typed store under its
		 * own period, UTC-stamped from the timebase exactly like a
		 * basic block (same generation-keyed sample mapping). */
		const auto aggregate_view = ingest.latest_decoded(
			msap1::MeasurementPeriod::Cycles150_180);
		require(aggregate_view.has_value() &&
			aggregate_view->latest_sequence == 4 &&
			aggregate_view->aggregate_timing.has_value(),
			"the aggregate period view was not published");
		const auto &timing = *aggregate_view->aggregate_timing;
		require(timing.time_quality ==
				msap1::meter::TimeQuality::Synchronized &&
			timing.utc_start.has_value() &&
			timing.utc_uncertainty_ns == 250,
			"aggregate timing was not UTC-stamped like basic timing");
		require(std::chrono::duration_cast<std::chrono::nanoseconds>(
				timing.utc_start->time_since_epoch())
					.count() == 1'700'000'000'000'000'000ll,
			"the aggregate UTC label was not mapped from its first sample");
		require(timing.cycle_count == 180 &&
			timing.basic_block_count == 15 &&
			timing.sample_count == 96'000,
			"the aggregate identity was not decoded into the view");
		const auto basic_view = ingest.latest_decoded(
			msap1::MeasurementPeriod::Basic);
		require(basic_view.has_value() &&
			basic_view->latest_sequence == 4 &&
			basic_view->timing.has_value() &&
			basic_view->timing->utc_start.has_value(),
			"basic decoding/stamping changed with aggregates present");

		/* Stale/out-of-order aggregates are invalid, not gaps... */
		feed(aggregate_record(2, 640'000, 0xfeedbeefu));
		require(ingest.invalid_records() == 1 &&
			ingest.aggregate_sequence_gaps() == 1,
			"a stale aggregate was not rejected");
		/* ...and a wrong generation is invalid, exactly as for basic
		 * records. */
		feed(aggregate_record(5, 640'000, 0x12345678u));
		require(ingest.invalid_records() == 2,
			"a stale-generation aggregate was accepted");

		/* A sync point from another generation must refuse the sample
		 * mapping (no UTC label) while quality stays Synchronized. */
		timebase.record_sync(
			{.sample_counter = 640'000,
			 .utc_ns = 1'700'000'100'000'000'000ll,
			 .uncertainty_ns = 250,
			 .sample_rate_hz = 32000,
			 .configuration_generation = 0x12345678u,
			 .utc_synchronized = true},
			std::chrono::steady_clock::now());
		feed(aggregate_record(5, 640'000, 0xfeedbeefu));
		const auto unmapped = ingest.latest_decoded(
			msap1::MeasurementPeriod::Cycles150_180);
		require(unmapped.has_value() &&
			unmapped->latest_sequence == 5 &&
			unmapped->aggregate_timing.has_value() &&
			!unmapped->aggregate_timing->utc_start.has_value() &&
			!unmapped->aggregate_timing->utc_uncertainty_ns
				 .has_value() &&
			unmapped->aggregate_timing->time_quality ==
				msap1::meter::TimeQuality::Synchronized,
			"a cross-generation sync point mislabeled an aggregate");

		/* Both formats reached the durable WAL stream: 4 basic + 4
		 * aggregate records were accepted. */
		require(stream.read_after(0, 32).size() == 8,
			"accepted records did not all reach the WAL stream");

		/* A configuration swap is a boundary for BOTH caches: an
		 * aggregate from the old generation must never outlive the
		 * basic record it was folded from. begin_epoch() clears the
		 * same state at a deliberate capture restart. */
		require(ingest.latest_aggregate_record().has_value(),
			"the aggregate cache was empty before the reset check");
		ingest.clear_latest();
		require(!ingest.latest_record().has_value() &&
			!ingest.latest_aggregate_record().has_value() &&
			ingest.aggregate_record_age_ms() ==
				msap1::acquisition_age_unavailable,
			"clear_latest() did not reset the aggregate cache");
		feed(aggregate_record(9, 640'000, 0xfeedbeefu));
		require(ingest.latest_aggregate_record().has_value(),
			"the aggregate cache did not refill after a swap");
		ingest.begin_epoch();
		require(!ingest.latest_aggregate_record().has_value() &&
			ingest.aggregate_record_age_ms() ==
				msap1::acquisition_age_unavailable,
			"begin_epoch() did not reset the aggregate cache");
	}
	std::filesystem::remove(database);
}

void ingestor_handles_u32_sequence_wrap()
{
	using msap1::acquisition::daemon::MeterRecordIngestor;
	const auto database = std::filesystem::temp_directory_path() /
		("msap1-ingestor-wrap-test-" + std::to_string(::getpid()) +
		 ".sqlite3");
	std::filesystem::remove(database);
	{
		ScriptedMeterSource source;
		msap1::MeterRecordStream stream(database);
		msap1::PreparedMeterConfiguration configuration{};
		configuration.wire.generation = 0xfeedbeefu;
		configuration.wire.sample_rate_hz = 32000;
		configuration.wire.rms_window_samples = 6400;
		const msap1::meter::MeasurementTimebase timebase;
		MeterRecordIngestor ingest(source, stream, configuration,
					   timebase);
		ingest.begin_epoch();

		const auto feed = [&](const msap1::MeterRecord &record) {
			source.next = {};
			source.next.records[0] = record;
			source.next.count = 1;
			source.next.bytes = sizeof(msap1::MeterRecord);
			ingest.read_available();
		};

		/* The 32-bit wire sequence wraps ~37 h @ 32 kSPS; the 64-bit
		 * sample counter keeps counting straight through it. */
		feed(v2_record(0xffffffffu, 10'000'000'000ull, 6400,
			       0xfeedbeefu));
		feed(v2_record(0u, 10'000'006'400ull, 6400, 0xfeedbeefu));
		require(ingest.meter_records() == 2 &&
			ingest.sequence_gaps() == 0 &&
			ingest.invalid_records() == 0,
			"uint32 sequence wraparound was mistaken for a gap");
	}
	std::filesystem::remove(database);
}

void malformed_and_unknown_requests_are_rejected()
{
	msap1::AcquisitionCommandRegistry registry;
	registry.on<msap1::InfoRequest>(msap1::AcquisitionStatus::dma_error,
		[](const msap1::InfoRequest &) {
			return msap1::InfoResponse{};
		});

	auto malformed = msap1::encode_acquisition_request(msap1::InfoRequest{});
	malformed.payload.resize(1);
	const auto rejected = registry.dispatch(malformed);
	require(rejected.kind == mnc::ipc::FrameKind::error,
		"malformed request must produce an error frame");
	const auto rejection =
		msap1::decode_acquisition_payload<msap1::InfoResponse>(rejected);
	require(rejection.status == msap1::AcquisitionStatus::bad_request,
		"malformed request must report bad_request");

	auto unknown = msap1::encode_acquisition_request(msap1::InfoRequest{});
	unknown.message_type = 0xdeadbeefu;
	const auto unknown_reply = registry.dispatch(unknown);
	require(unknown_reply.kind == mnc::ipc::FrameKind::error,
		"unknown command must produce an error frame");
}

} // namespace

int main()
{
	device_interfaces_are_substitutable();
	typed_commands_round_trip_through_the_registry();
	ingestor_validates_v2_sample_range_continuity();
	ingestor_tracks_interleaved_aggregate_stream();
	ingestor_handles_u32_sequence_wrap();
	malformed_and_unknown_requests_are_rejected();
	return 0;
}
