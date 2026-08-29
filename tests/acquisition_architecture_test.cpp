#include "msap1/acquisition/dma/meter_record_source.hpp"
#include "msap1/acquisition/rpu/rpu_control.hpp"
#include "msap1/acquisition/ipc/acquisition_ipc.hpp"
#include "pipeline/record_interval_category.hpp"
#include "pipeline/record_ingestor.hpp"
#include "msap1/meter/energy_ledger.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <span>
#include <string>
#include <string_view>


namespace {

void require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

void rejection_interval_categories_cover_meter_tiers()
{
	using msap1::acquisition::daemon::record_interval_identity;
	struct Case {
		std::uint32_t format;
		std::uint32_t harmonic_period;
		std::string_view code;
		std::string_view label;
	};
	constexpr std::array cases{
		Case{msap1::meter_periodic_format, 0, "basic", "10/12-cycle"},
		Case{msap1::meter_harmonic_format, 0, "basic", "10/12-cycle"},
		Case{msap1::meter_aggregate_format, 0, "cycles_150_180",
			"150/180-cycle"},
		Case{msap1::meter_ten_minute_open_format, 0, "minutes_10_live",
			"10-minute live partial"},
		Case{msap1::meter_harmonic_aggregate_format, 1,
			"cycles_150_180", "150/180-cycle"},
		Case{msap1::meter_harmonic_aggregate_format, 2, "minutes_10",
			"10-minute"},
		Case{msap1::meter_harmonic_aggregate_format, 3, "hours_2",
			"2-hour"},
	};

	for (const auto &test : cases) {
		msap1::MeterRecord record{};
		record.words[1] = test.format;
		record.words[14] = test.harmonic_period;
		const auto identity = record_interval_identity(record);
		require(identity.code == test.code && identity.label == test.label,
			"meter record interval category is wrong");
	}

	msap1::MeterRecord malformed_aggregate{};
	malformed_aggregate.words[1] = msap1::meter_harmonic_aggregate_format;
	const auto unknown = record_interval_identity(malformed_aggregate);
	require(unknown.code == "unknown" && unknown.label == "unknown interval",
		"malformed harmonic aggregate period was guessed");
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
	/* Only the whole-status accessor is overridden: transport_overruns()
	 * must keep working by projecting out of it, which is what lets a
	 * source implement the kernel accounting exactly once. */
	msap1::acquisition::MeterTransportStatus transport_status() noexcept override
	{
		return {12345, 12340, 5, 12290, 64};
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

class FakeRecordPublisher final
	: public mnc::meter_stream::MeterRecordPublisher {
public:
	std::uint64_t publish(
		const mnc::meter_stream::MeterStreamRecord &record) override
	{
		records.push_back(record);
		return records.size();
	}
	std::vector<std::uint64_t> publish_records(
		std::span<const mnc::meter_stream::MeterStreamRecord> family) override
	{
		std::vector<std::uint64_t> cursors;
		cursors.reserve(family.size());
		for (const auto &record : family)
			cursors.push_back(publish(record));
		return cursors;
	}

	std::vector<mnc::meter_stream::MeterStreamRecord> records;
};

class FailingRecordPublisher final
	: public mnc::meter_stream::MeterRecordPublisher {
public:
	explicit FailingRecordPublisher(std::size_t fail_on) : fail_on_(fail_on) {}
	std::uint64_t publish(
		const mnc::meter_stream::MeterStreamRecord &) override
	{
		if (++attempts_ == fail_on_)
			throw std::runtime_error("deliberate spool failure");
		return attempts_;
	}
	std::vector<std::uint64_t> publish_records(
		std::span<const mnc::meter_stream::MeterStreamRecord> records) override
	{
		if (++attempts_ == fail_on_)
			throw std::runtime_error("deliberate spool failure");
		return std::vector<std::uint64_t>(records.size(), attempts_);
	}
	std::size_t attempts_ = 0;
	std::size_t fail_on_ = 0;
};

class ConflictingM17Publisher final
	: public mnc::meter_stream::MeterRecordPublisher {
public:
	std::uint64_t publish(
		const mnc::meter_stream::MeterStreamRecord &record) override
	{
		if (record.record_format == msap1::meter_demand_format &&
		    demand_conflict_pending) {
			demand_conflict_pending = false;
			throw msap1::energy_ledger::Conflict(
				"stale DEMAND family sequence");
		}
		records.push_back(record);
		return records.size();
	}

	std::vector<std::uint64_t> publish_records(
		std::span<const mnc::meter_stream::MeterStreamRecord> family) override
	{
		if (!family.empty() &&
		    family.front().record_format == msap1::meter_energy_format &&
		    energy_conflict_pending) {
			energy_conflict_pending = false;
			throw msap1::energy_ledger::Conflict(
				"stale ENERGY family sequence");
		}
		std::vector<std::uint64_t> cursors;
		cursors.reserve(family.size());
		for (const auto &record : family)
			cursors.push_back(publish(record));
		return cursors;
	}

	std::vector<mnc::meter_stream::MeterStreamRecord> records;
	bool energy_conflict_pending = true;
	bool demand_conflict_pending = true;
};

void device_interfaces_are_substitutable()
{
	FakeMeterSource meter;
	msap1::acquisition::MeterRecordSource &source = meter;
	source.start();
	require(meter.started, "meter source did not start");
	require(source.native_handle() == 17, "wrong fake descriptor");
	require(source.read_available().count == 1, "wrong fake batch");
	const auto transport = source.transport_status();
	require(transport.produced_blocks == 12345 &&
			transport.consumed_blocks == 12340 &&
			transport.overrun_blocks == 5 &&
			transport.callbacks == 12290 &&
			transport.ring_blocks == 64,
		"wrong fake transport status");
	require(source.transport_overruns() == 5,
		"transport_overruns did not project out of transport_status");
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
			/* Deliberately different: the daemon's live clock
			 * state and the cached aggregate's ingest-time
			 * provenance are distinct wire fields. */
			response.time_quality =
				msap1::meter::TimeQuality::Holdover;
			response.aggregate_time_quality =
				msap1::meter::TimeQuality::Synchronized;
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
	require(response.time_quality == msap1::meter::TimeQuality::Holdover &&
		response.aggregate_time_quality ==
			msap1::meter::TimeQuality::Synchronized,
		"the two time-quality fields did not round trip separately");

	/* IPC v20 carries reusable period/attribute selection and typed values,
	 * including the optional attribute index reserved for harmonics. */
	registry.on<msap1::MeterSnapshotRequest>(
		msap1::AcquisitionStatus::internal_error,
		[](const msap1::MeterSnapshotRequest &request) {
			require(request.selection.period ==
				mnc::meter::MeasurementPeriod::Cycles150_180,
				"wrong decoded meter period");
			require(request.selection.attributes.size() == 2 &&
				request.selection.attributes[1].index == 5,
				"wrong decoded meter attribute selection");
			msap1::MeterSnapshotResponse response{};
			response.has_snapshot = true;
			response.snapshot.period = request.selection.period;
			response.snapshot.sequence = 0x1'0000'0002ull;
			response.snapshot.timing = mnc::meter::MeterSnapshotTiming{
				.quality = mnc::meter::TimeQuality::Synchronized,
				.utc_start_nanoseconds = 1'700'000'000'000'000'000ll,
				.utc_uncertainty_nanoseconds = 250,
				.first_sample_index = 123456,
				.sample_count = 7680,
				.cycle_count = 12,
				.nominal_frequency_hz = 60,
			};
			response.snapshot.values.push_back({
				.attribute = request.selection.attributes.front(),
				.unit = mnc::meter::MeterUnit::MicroVolts,
				.quality = mnc::meter::ReadingQuality::Valid,
				.value = 120'000'000,
			});
			return response;
		});

	msap1::MeterSnapshotRequest snapshot_request{};
	snapshot_request.selection.period =
		mnc::meter::MeasurementPeriod::Cycles150_180;
	snapshot_request.selection.attributes = {
		{mnc::meter::MeterAttributeId::VanRms, std::nullopt},
		{mnc::meter::MeterAttributeId::Frequency, 5},
	};
	auto snapshot_frame =
		msap1::encode_acquisition_request(snapshot_request);
	snapshot_frame.correlation_id = 0xaabbccdd;
	const auto snapshot_reply = registry.dispatch(snapshot_frame);
	const auto snapshot_response =
		msap1::decode_acquisition_payload<msap1::MeterSnapshotResponse>(
			snapshot_reply);
	require(snapshot_response.has_snapshot &&
		snapshot_response.snapshot.sequence == 0x1'0000'0002ull &&
		snapshot_response.snapshot.timing &&
		snapshot_response.snapshot.timing->quality ==
			mnc::meter::TimeQuality::Synchronized &&
		snapshot_response.snapshot.timing->first_sample_index == 123456 &&
		snapshot_response.snapshot.timing->cycle_count == 12 &&
		snapshot_response.snapshot.values.size() == 1 &&
		snapshot_response.snapshot.values[0].value == 120'000'000,
		"typed meter snapshot did not round trip");

	/* IPC v32 carries a complete, already-atomic M16 family. */
	registry.on<msap1::HarmonicRequest>(
		msap1::AcquisitionStatus::internal_error,
		[](const msap1::HarmonicRequest &request) {
			require(request.version == msap1::acquisition_ipc_version,
				"wrong decoded harmonic request version");
			msap1::HarmonicResponse response{};
			response.running = true;
			response.records = 84;
			response.families = 2;
			response.incomplete_families = 1;
			response.has_snapshot = true;
			response.snapshot.sequence = 0x12345678u;
			response.snapshot.qualified_max_order = 127;
			response.snapshot.channels[6][126] = {
				.order = 127,
				.magnitude_micro_units = 230'000'000,
				.angle_millidegrees = 359'999,
				.magnitude_valid = true,
				.angle_valid = true,
			};
			return response;
		});
	const auto harmonic_reply = registry.dispatch(
		msap1::encode_acquisition_request(msap1::HarmonicRequest{}));
	const auto harmonic_response =
		msap1::decode_acquisition_payload<msap1::HarmonicResponse>(
			harmonic_reply);
	const auto &order_127 = harmonic_response.snapshot.channels[6][126];
	require(harmonic_response.running && harmonic_response.records == 84 &&
		harmonic_response.families == 2 &&
		harmonic_response.incomplete_families == 1 &&
		harmonic_response.has_snapshot &&
		harmonic_response.snapshot.sequence == 0x12345678u &&
		order_127.order == 127 &&
		order_127.magnitude_micro_units == 230'000'000 &&
		order_127.angle_millidegrees == 359'999 &&
		order_127.magnitude_valid && order_127.angle_valid,
		"typed harmonic family did not round trip");
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

/* Minimal valid basic (MTR1) record for the continuity checks: first-sample
 * index in envelope words 9/10, timing word 13; channel words stay zero
 * because validation never inspects the electrical payload. */
msap1::MeterRecord basic_record(std::uint32_t sequence,
				std::uint64_t first_sample_index,
				std::uint32_t sample_count,
				std::uint32_t generation,
				bool utc_resynchronized = false)
{
	msap1::MeterRecord record{};
	record.words[0] = msap1::meter_record_magic;
	record.words[1] = msap1::meter_periodic_format;
	record.words[2] = msap1::meter_record_size;
	record.words[3] = sequence;
	record.words[4] = generation;
	record.words[5] = 32000;
	record.words[6] = sample_count;
	record.words[9] = static_cast<std::uint32_t>(first_sample_index);
	record.words[10] = static_cast<std::uint32_t>(first_sample_index >> 32);
	record.words[13] = 60u | (12u << 8) | (1u << 16) |
		(utc_resynchronized ? (1u << 19) : 0u);
	return record;
}

void write_record_u64(msap1::MeterRecord &record, std::size_t word,
	std::uint64_t value)
{
	record.words[word] = static_cast<std::uint32_t>(value);
	record.words[word + 1U] = static_cast<std::uint32_t>(value >> 32U);
}

msap1::MeterRecord energy_record(std::uint32_t sequence, std::uint8_t part,
	std::uint32_t generation, std::uint64_t session_id)
{
	msap1::MeterRecord record{};
	record.words[0] = msap1::meter_record_magic;
	record.words[1] = msap1::meter_energy_format;
	record.words[2] = msap1::meter_record_size;
	record.words[3] = sequence;
	record.words[4] = generation;
	record.words[5] = 32000U;
	record.words[6] = 6400U;
	record.words[7] = 0x7fU;
	record.words[8] = 1U << 1U;
	write_record_u64(record, 9U, 640000U);
	record.words[13] = part | (2U << 2U) | (1U << 4U) | (0xfU << 8U);
	write_record_u64(record, 14U, 646399U);
	if (part == msap1::meter_energy_part_summary) {
		for (const auto base : {msap1::meter_energy_summary_import_word,
			msap1::meter_energy_summary_export_word,
			msap1::meter_energy_summary_apparent_word})
			for (std::size_t index = 0; index < 4U; ++index)
				write_record_u64(record, base + index * 2U,
					100U + base + index);
	} else {
		for (const auto base : msap1::meter_energy_quadrant_words)
			for (std::size_t index = 0; index < 4U; ++index)
				write_record_u64(record, base + index * 2U,
					100U + base + index);
	}
	write_record_u64(record, msap1::meter_energy_session_word, session_id);
	write_record_u64(record, msap1::meter_energy_accepted_samples_word,
		6400U);
	record.words[msap1::meter_energy_accepted_blocks_word] = 1U;
	return record;
}

msap1::MeterRecord demand_record(std::uint32_t sequence,
	std::uint32_t generation, std::uint64_t session_id)
{
	msap1::MeterRecord record{};
	record.words[0] = msap1::meter_record_magic;
	record.words[1] = msap1::meter_demand_format;
	record.words[2] = msap1::meter_record_size;
	record.words[3] = sequence;
	record.words[4] = generation;
	record.words[5] = 32000U;
	record.words[6] = 19'200'000U;
	record.words[7] = 0x7fU;
	record.words[8] = (1U << 1U) | (1U << 2U) | (1U << 4U);
	write_record_u64(record, 9U, 1'000'000U);
	record.words[13] = msap1::meter_demand_fixed_interval_seconds |
		(0xfU << 16U) |
		(static_cast<std::uint32_t>(
			msap1::meter_demand_fixed_interval_seconds) << 22U);
	write_record_u64(record, msap1::meter_demand_last_sample_word,
		20'199'999U);
	for (std::size_t index = 0; index < 4U; ++index) {
		write_record_u64(record,
			msap1::meter_demand_current_word + index * 2U,
			1000U + index);
		write_record_u64(record,
			msap1::meter_demand_import_peak_word + index * 2U,
			2000U + index);
		write_record_u64(record,
			msap1::meter_demand_export_peak_word + index * 2U,
			3000U + index);
		write_record_u64(record,
			msap1::meter_demand_import_peak_anchor_word + index * 2U,
			100U + index);
		write_record_u64(record,
			msap1::meter_demand_export_peak_anchor_word + index * 2U,
			200U + index);
	}
	write_record_u64(record, msap1::meter_demand_session_word, session_id);
	write_record_u64(record, msap1::meter_demand_interval_anchor_sample_word,
		20'200'000U);
	record.words[msap1::meter_demand_source_interval_count_word] = 1U;
	record.words[msap1::meter_demand_source_status_word] = record.words[8];
	record.words[msap1::meter_demand_profile_generation_word] = 1U;
	return record;
}

msap1::MeterRecord pq_lifecycle_record(std::uint32_t sequence,
	std::uint32_t generation, std::uint64_t first, std::uint64_t last)
{
	msap1::MeterRecord record{};
	record.words[0] = msap1::meter_record_magic;
	record.words[1] = msap1::meter_pq_event_lifecycle_format;
	record.words[2] = msap1::meter_record_size;
	record.words[3] = sequence;
	record.words[4] = generation;
	record.words[5] = 32000;
	record.words[6] = static_cast<std::uint32_t>(last - first + 1u);
	record.words[7] = 0x10;
	record.words[8] = 0x0a;
	write_record_u64(record, 9, first);
	record.words[13] = msap1::meter_event_lifecycle_update | (1u << 8u);
	write_record_u64(record, 14, last);
	write_record_u64(record, 16, 0x123456789abcdef0ull);
	write_record_u64(record, 18, 8);
	record.words[20] = generation;
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
	write_record_u64(record, 37, last - first);
	write_record_u64(record, 39, first);
	record.words[47] = sequence;
	record.words[48] = 1;
	record.words[49] = 2;
	record.words[50] = 3;
	record.words[51] = 4;
	return record;
}

void ingestor_isolates_and_publishes_pq_lifecycle_records()
{
	using msap1::acquisition::daemon::MeterRecordIngestor;
	ScriptedMeterSource source;
	msap1::PreparedMeterConfiguration configuration{};
	configuration.wire.generation = 0xfeedbeefU;
	configuration.wire.sample_rate_hz = 32000U;
	const msap1::meter::MeasurementTimebase timebase;
	FakeRecordPublisher publisher;
	MeterRecordIngestor ingest(source, configuration, timebase, publisher);
	ingest.begin_epoch();
	const auto feed = [&](const msap1::MeterRecord &record) {
		source.next = {};
		source.next.records[0] = record;
		source.next.count = 1U;
		source.next.bytes = sizeof(msap1::MeterRecord);
		ingest.read_available();
	};

	feed(pq_lifecycle_record(1, configuration.wire.generation, 1000, 1100));
	auto malformed = pq_lifecycle_record(
		2, configuration.wire.generation, 1101, 1200);
	malformed.words[63] = 1;
	feed(malformed);
	feed(pq_lifecycle_record(3, configuration.wire.generation, 1201, 1300));
	feed(basic_record(1, 2000, 100, configuration.wire.generation));
	feed(basic_record(2, 2100, 100, configuration.wire.generation));

	require(ingest.pq_lifecycle_records() == 2 &&
			ingest.pq_lifecycle_sequence_gaps() == 1 &&
			ingest.invalid_records() == 1,
		"PQ lifecycle decoder/continuity did not quarantine one malformed edge");
	require(ingest.sequence_gaps() == 0 &&
			ingest.latest_record()->sequence() == 2,
		"a malformed PQ lifecycle edge poisoned BASIC continuity");
	require(ingest.latest_pq_lifecycle()->sequence == 3 &&
			publisher.records.size() == 4 &&
			publisher.records.front().record_kind == static_cast<std::uint16_t>(
				msap1::RecordKind::power_quality_event),
		"validated lifecycle edges did not cross the durability barrier");
}

void ingestor_quarantines_m17_ledger_conflicts()
{
	using msap1::acquisition::daemon::MeterRecordIngestor;
	ScriptedMeterSource source;
	msap1::PreparedMeterConfiguration configuration{};
	configuration.wire.generation = 0xfeedbeefU;
	configuration.wire.sample_rate_hz = 32000U;
	configuration.wire.rms_window_samples = 6400U;
	const msap1::meter::MeasurementTimebase timebase;
	ConflictingM17Publisher publisher;
	MeterRecordIngestor ingest(source, configuration, timebase, publisher);
	ingest.begin_epoch();

	const auto feed = [&](const msap1::MeterRecord &record) {
		source.next = {};
		source.next.records[0] = record;
		source.next.count = 1U;
		source.next.bytes = sizeof(msap1::MeterRecord);
		ingest.read_available();
	};
	constexpr std::uint64_t repeated_boot_session =
		0xd78c5e6a33d83a55ULL;
	constexpr std::uint64_t fresh_boot_session = 0x123456789abcdef0ULL;
	feed(energy_record(1U, msap1::meter_energy_part_summary,
		configuration.wire.generation, repeated_boot_session));
	feed(energy_record(1U, msap1::meter_energy_part_quadrants,
		configuration.wire.generation, repeated_boot_session));
	feed(energy_record(1U, msap1::meter_energy_part_summary,
		configuration.wire.generation, fresh_boot_session));
	feed(energy_record(1U, msap1::meter_energy_part_quadrants,
		configuration.wire.generation, fresh_boot_session));
	feed(basic_record(1U, 640000U, 6400U,
		configuration.wire.generation));
	feed(demand_record(1U, configuration.wire.generation,
		repeated_boot_session));
	feed(demand_record(1U, configuration.wire.generation,
		fresh_boot_session));
	feed(basic_record(2U, 646400U, 6400U,
		configuration.wire.generation));

	require(ingest.energy_families() == 1U &&
		ingest.incomplete_energy_families() == 1U,
		"ENERGY did not quarantine the collision then accept a fresh session");
	require(ingest.invalid_records() == 2U &&
		ingest.lifetime_invalid_records() == 2U,
		"M17 ledger conflicts were not retained as diagnostics");
	require(ingest.latest_record().has_value() &&
		ingest.latest_record()->sequence() == 2U &&
		publisher.records.size() == 5U,
		"an M17 ledger conflict stopped ordinary meter acquisition");
}

msap1::MeterRecord harmonic_record(std::uint32_t sequence,
				   std::uint8_t channel, std::uint8_t chunk,
				   std::uint32_t generation)
{
	msap1::MeterRecord record{};
	record.words[0] = msap1::meter_record_magic;
	record.words[1] = msap1::meter_harmonic_format;
	record.words[2] = msap1::meter_record_size;
	record.words[3] = sequence;
	record.words[4] = generation;
	record.words[5] = 128000;
	record.words[6] = 25600;
	record.words[7] = 0x7f;
	record.words[8] = 0x3e; /* complete, locked, conditioned, FFT, full */
	record.words[9] = 1'000'000;
	const auto first = static_cast<std::uint32_t>(chunk) * 24u + 1u;
	const auto count = std::min(24u, 128u - first);
	record.words[13] = channel | (static_cast<std::uint32_t>(chunk) << 3) |
		(first << 7) | (count << 15) | (6u << 20) | (127u << 24);
	record.words[14] = 50000;
	record.words[15] = 127u | (50u << 8) | (10u << 16) | (1u << 24);
	for (std::uint32_t entry = 0; entry < count; ++entry) {
		const std::uint64_t packed = (first + entry) |
			(std::uint64_t{1} << 60);
		record.words[16 + entry * 2] = static_cast<std::uint32_t>(packed);
		record.words[17 + entry * 2] =
			static_cast<std::uint32_t>(packed >> 32);
	}
	return record;
}

void ingestor_publishes_only_complete_harmonic_families()
{
	using msap1::acquisition::daemon::MeterRecordIngestor;
	ScriptedMeterSource source;
	msap1::PreparedMeterConfiguration configuration{};
	configuration.wire.generation = 0xfeedbeefu;
	configuration.wire.sample_rate_hz = 128000;
	const msap1::meter::MeasurementTimebase timebase;
	FakeRecordPublisher publisher;
	MeterRecordIngestor ingest(source, configuration, timebase, publisher);
	ingest.begin_epoch();
	const auto feed = [&](std::uint8_t channel, std::uint8_t chunk) {
		source.next = {};
		source.next.records[0] = harmonic_record(
			9, channel, chunk, configuration.wire.generation);
		source.next.count = 1;
		source.next.bytes = sizeof(msap1::MeterRecord);
		ingest.read_available();
	};
	for (std::uint8_t channel = 0; channel < 7; ++channel)
		for (std::uint8_t chunk = 0; chunk < 6; ++chunk) {
			if (channel == 6 && chunk == 5)
				continue;
			feed(channel, chunk);
		}
	require(publisher.records.empty() &&
			!ingest.latest_harmonic_spectrum().has_value(),
		"a partial harmonic family entered the spool or latest state");
	feed(6, 5);
	require(publisher.records.size() == 42 &&
			ingest.latest_harmonic_spectrum().has_value() &&
			ingest.harmonic_records() == 42 &&
			ingest.harmonic_families() == 1,
		"the final durably published chunk did not expose the family");
	require(publisher.records.back().record_format ==
			msap1::meter_harmonic_format &&
			publisher.records.back().record_kind ==
				static_cast<std::uint16_t>(msap1::RecordKind::harmonic) &&
			publisher.records.back().payload.size() ==
				sizeof(msap1::MeterRecord),
		"harmonic spool envelope lost its exact record identity");

	/* A failed durability barrier on the completing chunk leaves latest empty. */
	ScriptedMeterSource failing_source;
	FailingRecordPublisher failing_publisher(1);
	MeterRecordIngestor failing(failing_source, configuration, timebase,
				    failing_publisher);
	failing.begin_epoch();
	bool threw = false;
	try {
		for (std::uint8_t channel = 0; channel < 7; ++channel)
			for (std::uint8_t chunk = 0; chunk < 6; ++chunk) {
				failing_source.next = {};
				failing_source.next.records[0] = harmonic_record(
					10, channel, chunk,
					configuration.wire.generation);
				failing_source.next.count = 1;
				failing_source.next.bytes = sizeof(msap1::MeterRecord);
				failing.read_available();
			}
	} catch (const std::runtime_error &) {
		threw = true;
	}
	require(threw && !failing.latest_harmonic_spectrum().has_value(),
		"latest crossed a failed harmonic durability barrier");
}

void ingestor_validates_sample_range_continuity()
{
	using msap1::acquisition::daemon::MeterRecordIngestor;
	{
		ScriptedMeterSource source;
		msap1::PreparedMeterConfiguration configuration{};
		configuration.wire.generation = 0xfeedbeefu;
		configuration.wire.sample_rate_hz = 32000;
		configuration.wire.rms_window_samples = 6400;
		const msap1::meter::MeasurementTimebase timebase;
		FakeRecordPublisher publisher;
		MeterRecordIngestor ingest(source, configuration, timebase,
			publisher);
		ingest.begin_epoch();

		const auto feed = [&](const msap1::MeterRecord &record) {
			source.next = {};
			source.next.records[0] = record;
			source.next.count = 1;
			source.next.bytes = sizeof(msap1::MeterRecord);
			ingest.read_available();
		};

		/* Gapless cycle blocks of varying length are accepted: word 6
		 * is the actual cycle-defined sample count, and there is no
		 * configured-window echo to match it against. */
		feed(basic_record(1, 640'000, 6400, 0xfeedbeefu));
		feed(basic_record(2, 646'400, 6421, 0xfeedbeefu));
		feed(basic_record(3, 652'821, 6379, 0xfeedbeefu));
		require(ingest.meter_records() == 3 &&
			ingest.sequence_gaps() == 0 &&
			ingest.invalid_records() == 0,
			"gapless variable-length blocks were not accepted");

		/* A first-sample discontinuity with continuous sequences is
		 * lost data: counted as a gap, then resynced. */
		feed(basic_record(4, 700'000, 6400, 0xfeedbeefu));
		require(ingest.meter_records() == 4 &&
			ingest.sequence_gaps() == 1,
			"a first-sample-index gap was not detected");
		feed(basic_record(5, 706'400, 6400, 0xfeedbeefu));
		require(ingest.sequence_gaps() == 1,
			"continuity did not resync after a sample-range gap");

		/* A wrong generation is invalid. */
		feed(basic_record(6, 712'800, 6400, 0x12345678u));
		require(ingest.invalid_records() == 1,
			"a stale-generation record was accepted");
	}

	{
		ScriptedMeterSource source;
		msap1::PreparedMeterConfiguration configuration{};
		configuration.wire.generation = 0xfeedbeefu;
		configuration.wire.sample_rate_hz = 32000;
		const msap1::meter::MeasurementTimebase timebase;
		FakeRecordPublisher publisher;
		MeterRecordIngestor ingest(source, configuration, timebase, publisher);
		ingest.begin_epoch();

		const auto feed = [&](const msap1::MeterRecord &record) {
			source.next = {};
			source.next.records[0] = record;
			source.next.count = 1;
			source.next.bytes = sizeof(msap1::MeterRecord);
			ingest.read_available();
		};

		feed(basic_record(1, 1'000, 120, 0xfeedbeefu));
		feed(basic_record(2, 1'060, 120, 0xfeedbeefu, true));
		require(ingest.sequence_gaps() == 0 &&
			ingest.latest_record()->timing().utc_resynchronized,
			"a marked lateral UTC overlap was reported as a sample gap");

		feed(basic_record(3, 1'120, 120, 0xfeedbeefu));
		require(ingest.sequence_gaps() == 1,
			"an unmarked overlap was accepted");

		/* A marker cannot hide a contained duplicate or a forward gap. */
		feed(basic_record(4, 1'130, 50, 0xfeedbeefu, true));
		require(ingest.sequence_gaps() == 2,
			"a contained marked duplicate was accepted");
		feed(basic_record(5, 2'000, 120, 0xfeedbeefu, true));
		require(ingest.sequence_gaps() == 3,
			"a marked forward sample gap was accepted");
	}
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
	record.words[9] = static_cast<std::uint32_t>(first_sample_index);
	record.words[10] = static_cast<std::uint32_t>(first_sample_index >> 32);
	record.words[13] = 15u | (60u << 8) | (180u << 16);
	record.words[14] = sequence * 15u - 14u;
	record.words[15] = sequence * 15u;
	record.words[32] = 60'000;
	const auto last_sample = first_sample_index + record.words[6] - 1u;
	record.words[36] = static_cast<std::uint32_t>(last_sample);
	record.words[37] = static_cast<std::uint32_t>(last_sample >> 32);
	return record;
}

/* Minimal valid clock-aligned M13 record. Its sequence starts independently
 * from both basic and 150/180-cycle streams at the first UTC boundary. */
msap1::MeterRecord ten_minute_record(std::uint32_t sequence,
				    std::uint32_t generation)
{
	msap1::MeterRecord record{};
	record.words[0] = msap1::meter_record_magic;
	record.words[1] = msap1::meter_ten_minute_format;
	record.words[2] = msap1::meter_record_size;
	record.words[3] = sequence;
	record.words[4] = generation;
	record.words[5] = 32000;
	record.words[6] = 19'200'000;
	record.words[7] = 0x7f;
	record.words[8] = (1u << 1) | (1u << 2) | (1u << 4);
	constexpr std::uint64_t first_sample = 5'000'001;
	record.words[9] = static_cast<std::uint32_t>(first_sample);
	record.words[10] = static_cast<std::uint32_t>(first_sample >> 32);
	record.words[13] = 3'000u | (60u << 16);
	record.words[14] = sequence * 3'000u - 2'999u;
	record.words[15] = sequence * 3'000u;
	const auto actual_last = first_sample + record.words[6] - 1u;
	constexpr std::uint32_t overshoot = 127;
	const auto target = actual_last - overshoot;
	record.words[36] = static_cast<std::uint32_t>(actual_last);
	record.words[37] = static_cast<std::uint32_t>(actual_last >> 32);
	record.words[41] = 36'000;
	record.words[42] = static_cast<std::uint32_t>(target);
	record.words[43] = static_cast<std::uint32_t>(target >> 32);
	record.words[44] = overshoot;
	return record;
}

/* Minimal valid M14 record. The sequence and contributing ten-minute span
 * belong to an independent stream, so neither may be compared with the
 * basic, 150/180-cycle, or ten-minute record counters. */
msap1::MeterRecord two_hour_record(std::uint32_t sequence,
				  std::uint32_t generation)
{
	msap1::MeterRecord record{};
	record.words[0] = msap1::meter_record_magic;
	record.words[1] = msap1::meter_two_hour_format;
	record.words[2] = msap1::meter_record_size;
	record.words[3] = sequence;
	record.words[4] = generation;
	record.words[5] = 32000;
	record.words[6] = 230'400'000;
	record.words[7] = 0x7f;
	record.words[8] = (1u << 1) | (1u << 2) | (1u << 4);
	constexpr std::uint64_t first_sample = 25'000'001;
	record.words[9] = static_cast<std::uint32_t>(first_sample);
	record.words[10] = static_cast<std::uint32_t>(first_sample >> 32);
	record.words[13] = 12u | (60u << 16);
	record.words[14] = sequence * 12u - 11u;
	record.words[15] = sequence * 12u;
	const auto actual_last = first_sample + record.words[6] - 1u;
	constexpr std::uint32_t overshoot = 127;
	const auto target = actual_last - overshoot;
	record.words[36] = static_cast<std::uint32_t>(actual_last);
	record.words[37] = static_cast<std::uint32_t>(actual_last >> 32);
	record.words[41] = 432'000;
	record.words[42] = static_cast<std::uint32_t>(target);
	record.words[43] = static_cast<std::uint32_t>(target >> 32);
	record.words[44] = overshoot;
	return record;
}

/* A UTC ten-minute boundary may occur between any two basic blocks. M13's
 * sequence 1 must not be compared with the already-advanced basic stream. */
void ingestor_tracks_ten_minute_stream_independently()
{
	using msap1::acquisition::daemon::MeterRecordIngestor;
	ScriptedMeterSource source;
	msap1::PreparedMeterConfiguration configuration{};
	configuration.wire.generation = 0xfeedbeefu;
	configuration.wire.sample_rate_hz = 32000;
	configuration.wire.rms_window_samples = 6400;
	const msap1::meter::MeasurementTimebase timebase;
	FakeRecordPublisher publisher;
	MeterRecordIngestor ingest(source, configuration, timebase, publisher);
	ingest.begin_epoch();

	const auto feed = [&](const msap1::MeterRecord &record) {
		source.next = {};
		source.next.records[0] = record;
		source.next.count = 1;
		source.next.bytes = sizeof(msap1::MeterRecord);
		ingest.read_available();
	};

	feed(basic_record(846, 1'000'000, 6400, 0xfeedbeefu));
	feed(ten_minute_record(1, 0xfeedbeefu));
	feed(basic_record(847, 1'006'400, 6400, 0xfeedbeefu));
	require(ingest.meter_records() == 3 &&
		ingest.invalid_records() == 0 && ingest.sequence_gaps() == 0 &&
		ingest.ten_minute_sequence_gaps() == 0,
		"ten-minute sequence 1 was compared with the basic stream");
	const auto ten_minute =
		ingest.latest_decoded(msap1::MeasurementPeriod::Min10);
	require(ten_minute && ten_minute->latest_sequence == 1,
		"the accepted ten-minute record was not published");

	feed(ten_minute_record(2, 0xfeedbeefu));
	feed(ten_minute_record(4, 0xfeedbeefu));
	require(ingest.ten_minute_sequence_gaps() == 1 &&
		ingest.sequence_gaps() == 0 &&
		ingest.aggregate_sequence_gaps() == 0,
		"ten-minute gaps were not isolated from other record streams");
}

void ingestor_tracks_two_hour_stream_independently()
{
	using msap1::acquisition::daemon::MeterRecordIngestor;
	ScriptedMeterSource source;
	msap1::PreparedMeterConfiguration configuration{};
	configuration.wire.generation = 0xfeedbeefu;
	configuration.wire.sample_rate_hz = 32000;
	configuration.wire.rms_window_samples = 6400;
	const msap1::meter::MeasurementTimebase timebase;
	FakeRecordPublisher publisher;
	MeterRecordIngestor ingest(source, configuration, timebase, publisher);
	ingest.begin_epoch();

	const auto feed = [&](const msap1::MeterRecord &record) {
		source.next = {};
		source.next.records[0] = record;
		source.next.count = 1;
		source.next.bytes = sizeof(msap1::MeterRecord);
		ingest.read_available();
	};

	feed(basic_record(846, 1'000'000, 6400, 0xfeedbeefu));
	feed(ten_minute_record(91, 0xfeedbeefu));
	feed(two_hour_record(1, 0xfeedbeefu));
	require(ingest.invalid_records() == 0 &&
		ingest.sequence_gaps() == 0 &&
		ingest.ten_minute_sequence_gaps() == 0 &&
		ingest.two_hour_sequence_gaps() == 0,
		"two-hour sequence 1 was compared with another stream");
	const auto two_hour =
		ingest.latest_decoded(msap1::MeasurementPeriod::Hour2);
	require(two_hour && two_hour->latest_sequence == 1,
		"the accepted two-hour record was not published");

	feed(two_hour_record(2, 0xfeedbeefu));
	feed(two_hour_record(4, 0xfeedbeefu));
	require(ingest.two_hour_sequence_gaps() == 1 &&
		ingest.sequence_gaps() == 0 &&
		ingest.aggregate_sequence_gaps() == 0 &&
		ingest.ten_minute_sequence_gaps() == 0,
		"two-hour gaps were not isolated from other record streams");
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
	{
		ScriptedMeterSource source;
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
		FakeRecordPublisher publisher;
		MeterRecordIngestor ingest(source, configuration, timebase,
			publisher);
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
		feed(basic_record(1, 640'000, 6400, 0xfeedbeefu));
		feed(basic_record(2, 646'400, 6400, 0xfeedbeefu));
		feed(aggregate_record(1, 640'000, 0xfeedbeefu));
		feed(basic_record(3, 652'800, 6400, 0xfeedbeefu));
		feed(aggregate_record(2, 640'000, 0xfeedbeefu));
		require(ingest.meter_records() == 5 &&
			ingest.sequence_gaps() == 0 &&
			ingest.aggregate_sequence_gaps() == 0 &&
			ingest.invalid_records() == 0,
			"interleaved records were mistaken for gaps");

		/* A missing aggregate is a gap on the AGGREGATE counter only;
		 * the basic counters must not move. */
		feed(aggregate_record(4, 640'000, 0xfeedbeefu));
		feed(basic_record(4, 659'200, 6400, 0xfeedbeefu));
		require(ingest.aggregate_sequence_gaps() == 1 &&
			ingest.sequence_gaps() == 0 &&
			ingest.meter_records() == 7,
			"an aggregate sequence gap was not isolated per format");

		/* The raw readings cache must remain the newest BASIC record
		 * even though an aggregate arrived after it. */
		require(ingest.latest_record().has_value() &&
			ingest.latest_record()->record_format() ==
				msap1::meter_periodic_format &&
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
			ingest.invalid_records() == 0 &&
			ingest.lifetime_invalid_records() == 2 &&
			ingest.aggregate_record_age_ms() ==
				msap1::acquisition_age_unavailable,
			"begin_epoch() did not reset epoch health while retaining diagnostics");
	}
}

/*
 * Timing provenance belongs to the MEASUREMENT, not to whenever a consumer
 * reads it back. The quality cached beside an aggregate is the one stamped
 * onto its decoded timing at ingest, so it must stay put when the timebase
 * later changes state — in either direction — and only move when a new
 * aggregate is ingested.
 */
void ingestor_pins_aggregate_time_quality_at_ingest()
{
	using msap1::acquisition::daemon::MeterRecordIngestor;
	using msap1::meter::TimeQuality;
	{
		ScriptedMeterSource source;
		msap1::PreparedMeterConfiguration configuration{};
		configuration.wire.generation = 0xfeedbeefu;
		configuration.wire.sample_rate_hz = 32000;
		configuration.wire.rms_window_samples = 6400;
		msap1::meter::MeasurementTimebase timebase;
		FakeRecordPublisher publisher;
		MeterRecordIngestor ingest(source, configuration, timebase,
			publisher);
		ingest.begin_epoch();

		const auto feed = [&](std::uint32_t sequence) {
			source.next = {};
			source.next.records[0] = aggregate_record(
				sequence,
				640'000 + (sequence - 1) * 96'000,
				0xfeedbeefu);
			source.next.count = 1;
			source.next.bytes = sizeof(msap1::MeterRecord);
			ingest.read_available();
		};
		/* A trusted sync disciplines the mapping; an untrusted one
		 * drops it back to Unsynchronized immediately, which is how
		 * this test moves the daemon's live quality without waiting
		 * out the holdover staleness threshold. */
		const auto sync = [&](bool utc_synchronized) {
			timebase.record_sync(
				{.sample_counter = 640'000,
				 .utc_ns = 1'700'000'000'000'000'000ll,
				 .uncertainty_ns = 250,
				 .sample_rate_hz = 32000,
				 .configuration_generation = 0xfeedbeefu,
				 .utc_synchronized = utc_synchronized},
				std::chrono::steady_clock::now());
		};

		sync(true);
		feed(1);
		require(ingest.latest_aggregate_time_quality() ==
			TimeQuality::Synchronized,
			"the ingest-time quality was not captured");

		/* The clock loses discipline AFTER the measurement: the
		 * finished aggregate keeps the label it was measured with. */
		sync(false);
		require(timebase.quality(std::chrono::steady_clock::now()) ==
				TimeQuality::Unsynchronized &&
			ingest.latest_aggregate_time_quality() ==
				TimeQuality::Synchronized,
			"the cached quality followed the daemon's later state");

		/* Only a new aggregate moves it. */
		feed(2);
		require(ingest.latest_aggregate_time_quality() ==
			TimeQuality::Unsynchronized,
			"a newly ingested aggregate did not restamp the quality");

		/* The other direction is just as wrong: regaining sync must
		 * not retroactively bless an unsynchronized measurement. */
		sync(true);
		require(timebase.quality(std::chrono::steady_clock::now()) ==
				TimeQuality::Synchronized &&
			ingest.latest_aggregate_time_quality() ==
				TimeQuality::Unsynchronized,
			"regained sync relabelled an older aggregate");
		feed(3);
		require(ingest.latest_aggregate_time_quality() ==
			TimeQuality::Synchronized,
			"the quality did not follow the newest aggregate");

		/* Provenance is reset with the cache it describes, at both
		 * deliberate boundaries, and never survives as a claim about
		 * a record that is gone. Unsynchronized is the conservative
		 * reset value. */
		ingest.clear_latest();
		require(!ingest.latest_aggregate_record().has_value() &&
			ingest.latest_aggregate_time_quality() ==
				TimeQuality::Unsynchronized,
			"clear_latest() left a stale aggregate time quality");
		feed(4);
		require(ingest.latest_aggregate_time_quality() ==
			TimeQuality::Synchronized,
			"the quality did not refill after a configuration swap");
		ingest.begin_epoch();
		require(!ingest.latest_aggregate_record().has_value() &&
			ingest.latest_aggregate_time_quality() ==
				TimeQuality::Unsynchronized,
			"begin_epoch() left a stale aggregate time quality");
	}
}

void ingestor_handles_u32_sequence_wrap()
{
	using msap1::acquisition::daemon::MeterRecordIngestor;
	{
		ScriptedMeterSource source;
		msap1::PreparedMeterConfiguration configuration{};
		configuration.wire.generation = 0xfeedbeefu;
		configuration.wire.sample_rate_hz = 32000;
		configuration.wire.rms_window_samples = 6400;
		const msap1::meter::MeasurementTimebase timebase;
		FakeRecordPublisher publisher;
		MeterRecordIngestor ingest(source, configuration, timebase,
			publisher);
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
		feed(basic_record(0xffffffffu, 10'000'000'000ull, 6400,
			       0xfeedbeefu));
		feed(basic_record(0u, 10'000'006'400ull, 6400, 0xfeedbeefu));
		require(ingest.meter_records() == 2 &&
			ingest.sequence_gaps() == 0 &&
			ingest.invalid_records() == 0,
			"uint32 sequence wraparound was mistaken for a gap");
	}
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
	rejection_interval_categories_cover_meter_tiers();
	device_interfaces_are_substitutable();
	typed_commands_round_trip_through_the_registry();
	ingestor_quarantines_m17_ledger_conflicts();
	ingestor_isolates_and_publishes_pq_lifecycle_records();
	ingestor_publishes_only_complete_harmonic_families();
	ingestor_validates_sample_range_continuity();
	ingestor_tracks_interleaved_aggregate_stream();
	ingestor_tracks_ten_minute_stream_independently();
	ingestor_tracks_two_hour_stream_independently();
	ingestor_pins_aggregate_time_quality_at_ingest();
	ingestor_handles_u32_sequence_wrap();
	malformed_and_unknown_requests_are_rejected();
	return 0;
}
