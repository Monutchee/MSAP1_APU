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
	ingestor_handles_u32_sequence_wrap();
	malformed_and_unknown_requests_are_rejected();
	return 0;
}
