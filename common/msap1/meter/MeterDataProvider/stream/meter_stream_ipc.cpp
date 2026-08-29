#include "msap1/meter/MeterDataProvider/stream/meter_stream_ipc.hpp"

#include <chrono>
#include <stdexcept>

namespace msap1::meter_stream {
namespace {

using mnc::ipc::ByteReader;
using mnc::ipc::ByteWriter;
using mnc::meter_stream::DatabaseStoragePolicy;
using mnc::meter_stream::RetentionPolicy;

void write_string(ByteWriter &writer, std::string_view value)
{
	writer.u32(static_cast<std::uint32_t>(value.size()));
	writer.bytes(mnc::ipc::to_payload(value));
}

std::string read_string(ByteReader &reader)
{
	const auto bytes = reader.bytes(reader.u32());
	return std::string(mnc::ipc::payload_view(bytes));
}

void encode_policy(ByteWriter &writer, const DatabaseStoragePolicy &policy)
{
	writer.u8(static_cast<std::uint8_t>(policy.dataset));
	writer.u8(static_cast<std::uint8_t>(policy.backend));
	writer.u16(0);
	writer.u64(policy.retention.maximum_age
		? static_cast<std::uint64_t>(policy.retention.maximum_age->count()) : 0);
	writer.u64(policy.retention.maximum_bytes.value_or(0));
}

DatabaseStoragePolicy decode_policy(ByteReader &reader)
{
	DatabaseStoragePolicy result;
	result.dataset = static_cast<mnc::meter_stream::DatabaseDataset>(reader.u8());
	result.backend = static_cast<mnc::meter_stream::StorageBackend>(reader.u8());
	(void)reader.u16();
	if (const auto age = reader.u64(); age != 0)
		result.retention.maximum_age = std::chrono::seconds(age);
	if (const auto bytes = reader.u64(); bytes != 0)
		result.retention.maximum_bytes = bytes;
	return result;
}

void require_ok(ByteReader &reader)
{
	const auto status = static_cast<Status>(reader.u32());
	if (status == Status::ok)
		return;
	const auto message = read_string(reader);
	if (status == Status::unavailable)
		throw energy_ledger::Unavailable(message);
	if (status == Status::conflict)
		throw energy_ledger::Conflict(message);
	if (status == Status::invalid_request)
		throw std::invalid_argument(message);
	throw std::runtime_error(message);
}

template<typename Unit>
void write_reading(ByteWriter &writer, const Reading<Unit> &reading)
{
	writer.i64(reading.value);
	writer.u8(static_cast<std::uint8_t>(reading.quality));
	writer.u8(0);
	writer.u16(0);
	writer.u64(reading.source_sequence);
	writer.i64(std::chrono::duration_cast<std::chrono::nanoseconds>(
		reading.measured_at.time_since_epoch()).count());
	writer.u32(reading.calculation_window.sample_count);
	writer.u32(0);
	writer.i64(reading.calculation_window.duration.count());
}

template<typename Unit>
Reading<Unit> read_reading(ByteReader &reader)
{
	Reading<Unit> result;
	result.value = reader.i64();
	const auto quality = reader.u8();
	if (quality > static_cast<std::uint8_t>(
		MeasurementQuality::arithmetic_error))
		throw std::invalid_argument(
			"invalid measurement quality in meter-stream snapshot");
	result.quality = static_cast<MeasurementQuality>(quality);
	if (reader.u8() != 0 || reader.u16() != 0)
		throw std::invalid_argument(
			"nonzero meter-stream snapshot reserved field");
	result.source_sequence = reader.u64();
	result.measured_at = SystemTime(std::chrono::nanoseconds(reader.i64()));
	result.calculation_window.sample_count = reader.u32();
	if (reader.u32() != 0)
		throw std::invalid_argument(
			"nonzero meter-stream snapshot reserved word");
	const auto duration = reader.i64();
	if (duration < 0)
		throw std::invalid_argument("negative meter-stream snapshot duration");
	result.calculation_window.duration = std::chrono::nanoseconds(duration);
	return result;
}

template<typename Unit>
void write_group(ByteWriter &writer,
	const PhaseABCTotal<Reading<Unit>> &group)
{
	write_reading(writer, group.phase_a);
	write_reading(writer, group.phase_b);
	write_reading(writer, group.phase_c);
	write_reading(writer, group.total);
}

template<typename Unit>
PhaseABCTotal<Reading<Unit>> read_group(ByteReader &reader)
{
	return {
		read_reading<Unit>(reader),
		read_reading<Unit>(reader),
		read_reading<Unit>(reader),
		read_reading<Unit>(reader),
	};
}

void write_samples(ByteWriter &writer,
	const PhaseABCTotal<std::uint64_t> &samples)
{
	writer.u64(samples.phase_a);
	writer.u64(samples.phase_b);
	writer.u64(samples.phase_c);
	writer.u64(samples.total);
}

PhaseABCTotal<std::uint64_t> read_samples(ByteReader &reader)
{
	return {reader.u64(), reader.u64(), reader.u64(), reader.u64()};
}

} // namespace

std::vector<std::byte> encode_record(
	const mnc::meter_stream::MeterStreamRecord &record)
{
	ByteWriter writer;
	writer.u64(record.cursor);
	writer.u32(record.record_format);
	writer.u16(record.record_kind);
	writer.u8(record.measurement_period);
	writer.u8(record.timing.time_quality);
	writer.u64(record.source_sequence);
	writer.u32(record.configuration_generation);
	writer.i64(record.ingested_at_nanoseconds);
	writer.u64(record.timing.first_sample_index);
	writer.u32(record.timing.sample_count);
	writer.u32(record.timing.cycle_count);
	writer.u8(record.timing.utc_start_nanoseconds.has_value());
	writer.u8(record.timing.utc_uncertainty_nanoseconds.has_value());
	/* This field was reserved as zero before fragmented producer families;
	 * consuming it preserves the record wire size and field offsets. */
	writer.u16(record.source_fragment);
	writer.i64(record.timing.utc_start_nanoseconds.value_or(0));
	writer.u64(record.timing.utc_uncertainty_nanoseconds.value_or(0));
	writer.u32(static_cast<std::uint32_t>(record.payload.size()));
	writer.bytes(record.payload);
	return writer.take();
}

mnc::meter_stream::MeterStreamRecord decode_record(ByteReader &reader)
{
	mnc::meter_stream::MeterStreamRecord record;
	record.cursor = reader.u64();
	record.record_format = reader.u32();
	record.record_kind = reader.u16();
	record.measurement_period = reader.u8();
	record.timing.time_quality = reader.u8();
	record.source_sequence = reader.u64();
	record.configuration_generation = reader.u32();
	record.ingested_at_nanoseconds = reader.i64();
	record.timing.first_sample_index = reader.u64();
	record.timing.sample_count = reader.u32();
	record.timing.cycle_count = reader.u32();
	const bool has_utc = reader.u8() != 0;
	const bool has_uncertainty = reader.u8() != 0;
	record.source_fragment = reader.u16();
	const auto utc = reader.i64();
	const auto uncertainty = reader.u64();
	if (has_utc)
		record.timing.utc_start_nanoseconds = utc;
	if (has_uncertainty)
		record.timing.utc_uncertainty_nanoseconds = uncertainty;
	record.payload = reader.bytes(reader.u32());
	return record;
}

std::vector<std::byte> encode_records(
	std::span<const mnc::meter_stream::MeterStreamRecord> records)
{
	if (records.empty() || records.size() > maximum_publish_records)
		throw std::invalid_argument(
			"meter-stream publish batch must contain 1..256 records");
	ByteWriter writer;
	writer.u32(static_cast<std::uint32_t>(records.size()));
	for (const auto &record : records)
		writer.bytes(encode_record(record));
	return writer.take();
}

std::vector<mnc::meter_stream::MeterStreamRecord> decode_records(
	ByteReader &reader)
{
	const auto count = reader.u32();
	if (count == 0 || count > maximum_publish_records)
		throw std::invalid_argument(
			"meter-stream publish batch must contain 1..256 records");
	std::vector<mnc::meter_stream::MeterStreamRecord> records;
	records.reserve(count);
	for (std::uint32_t index = 0; index < count; ++index)
		records.push_back(decode_record(reader));
	return records;
}

std::vector<std::byte> encode_energy_values(const EnergyValues &values)
{
	ByteWriter writer;
	write_group(writer, values.active_import);
	write_group(writer, values.active_export);
	write_group(writer, values.apparent);
	for (const auto &quadrant : values.reactive_quadrants)
		write_group(writer, quadrant);
	writer.u64(values.session_id);
	writer.u64(values.last_sample_index);
	writer.u64(values.accepted_samples);
	writer.u64(values.skipped_samples);
	writer.u32(values.accepted_blocks);
	writer.u32(values.skipped_blocks);
	writer.u64(values.reset_epoch);
	writer.u32(static_cast<std::uint32_t>(values.saturated) |
		(static_cast<std::uint32_t>(values.incomplete_input) << 1));
	writer.u32(0);
	return writer.take();
}

EnergyValues decode_energy_values(ByteReader &reader)
{
	EnergyValues result;
	result.active_import = read_group<MicroWattHours>(reader);
	result.active_export = read_group<MicroWattHours>(reader);
	result.apparent = read_group<MicroVoltAmpereHours>(reader);
	for (auto &quadrant : result.reactive_quadrants)
		quadrant = read_group<MicroVarHours>(reader);
	result.session_id = reader.u64();
	result.last_sample_index = reader.u64();
	result.accepted_samples = reader.u64();
	result.skipped_samples = reader.u64();
	result.accepted_blocks = reader.u32();
	result.skipped_blocks = reader.u32();
	result.reset_epoch = reader.u64();
	const auto flags = reader.u32();
	if ((flags & ~0x3u) != 0 || reader.u32() != 0)
		throw std::invalid_argument("invalid ENERGY snapshot flags");
	result.saturated = (flags & 1u) != 0;
	result.incomplete_input = (flags & 2u) != 0;
	return result;
}

std::vector<std::byte> encode_demand_values(const DemandValues &values)
{
	ByteWriter writer;
	write_group(writer, values.current_active);
	write_group(writer, values.import_peak);
	write_group(writer, values.export_peak);
	write_samples(writer, values.import_peak_sample);
	write_samples(writer, values.export_peak_sample);
	writer.u64(values.session_id);
	writer.u64(values.last_sample_index);
	writer.u64(values.interval_anchor_sample);
	writer.u32(values.source_interval_count);
	writer.u32(values.source_status);
	writer.u64(values.peak_reset_epoch);
	writer.u32(static_cast<std::uint32_t>(values.method));
	writer.u32(values.window_seconds);
	writer.u32(values.update_seconds);
	writer.u32(values.profile_generation);
	writer.u32(static_cast<std::uint32_t>(values.time_aligned) |
		(static_cast<std::uint32_t>(values.contaminated) << 1) |
		(static_cast<std::uint32_t>(values.boundary_valid) << 2) |
		(static_cast<std::uint32_t>(values.saturated) << 3) |
		(static_cast<std::uint32_t>(values.incomplete_input) << 4));
	writer.u32(0);
	return writer.take();
}

DemandValues decode_demand_values(ByteReader &reader)
{
	DemandValues result;
	result.current_active = read_group<MicroWatts>(reader);
	result.import_peak = read_group<MicroWatts>(reader);
	result.export_peak = read_group<MicroWatts>(reader);
	result.import_peak_sample = read_samples(reader);
	result.export_peak_sample = read_samples(reader);
	result.session_id = reader.u64();
	result.last_sample_index = reader.u64();
	result.interval_anchor_sample = reader.u64();
	result.source_interval_count = reader.u32();
	result.source_status = reader.u32();
	result.peak_reset_epoch = reader.u64();
	const auto method = reader.u32();
	if (method > static_cast<std::uint32_t>(DemandMethod::sliding))
		throw std::invalid_argument("invalid DEMAND snapshot method");
	result.method = static_cast<DemandMethod>(method);
	result.window_seconds = reader.u32();
	result.update_seconds = reader.u32();
	result.profile_generation = reader.u32();
	const auto flags = reader.u32();
	if ((flags & ~0x1fu) != 0 || reader.u32() != 0)
		throw std::invalid_argument("invalid DEMAND snapshot flags");
	result.time_aligned = (flags & 1u) != 0;
	result.contaminated = (flags & 2u) != 0;
	result.boundary_valid = (flags & 4u) != 0;
	result.saturated = (flags & 8u) != 0;
	result.incomplete_input = (flags & 16u) != 0;
	return result;
}

MeterRecordStreamClient::MeterRecordStreamClient(std::string path)
	: transport_(std::make_unique<mnc::ipc::PersistentBlockingClient>(
		std::move(path), connection_limits))
{
}

mnc::ipc::Frame MeterRecordStreamClient::request(Command command,
	std::vector<std::byte> payload, int timeout_ms) const
{
	mnc::ipc::Frame frame;
	frame.kind = mnc::ipc::FrameKind::request;
	frame.message_type = static_cast<std::uint32_t>(command);
	frame.payload = std::move(payload);
	return transport_->request(std::move(frame), timeout_ms);
}

std::uint64_t MeterRecordStreamClient::publish(
	const mnc::meter_stream::MeterStreamRecord &record)
{
	auto response = request(Command::publish_record, encode_record(record));
	ByteReader reader(response.payload);
	require_ok(reader);
	const auto cursor = reader.u64();
	reader.require_finished();
	return cursor;
}

std::vector<std::uint64_t> MeterRecordStreamClient::publish_records(
	std::span<const mnc::meter_stream::MeterStreamRecord> records)
{
	auto response = request(Command::publish_records, encode_records(records),
		10000);
	ByteReader reader(response.payload);
	require_ok(reader);
	const auto count = reader.u32();
	if (count != records.size())
		throw std::runtime_error(
			"meter-stream publish batch cursor count mismatch");
	std::vector<std::uint64_t> cursors;
	cursors.reserve(count);
	for (std::uint32_t index = 0; index < count; ++index)
		cursors.push_back(reader.u64());
	reader.require_finished();
	return cursors;
}

void MeterRecordStreamClient::register_consumer(std::string_view name)
{
	ByteWriter writer;
	write_string(writer, name);
	auto response = request(Command::register_consumer, writer.take());
	ByteReader reader(response.payload);
	require_ok(reader);
	reader.require_finished();
}

void MeterRecordStreamClient::unregister_consumer(std::string_view name)
{
	ByteWriter writer; write_string(writer, name);
	auto response = request(Command::unregister_consumer, writer.take());
	ByteReader reader(response.payload); require_ok(reader);
	reader.require_finished();
}

std::vector<mnc::meter_stream::MeterStreamRecord>
MeterRecordStreamClient::read_after(std::string_view name, std::size_t limit)
{
	ByteWriter writer;
	write_string(writer, name);
	writer.u32(static_cast<std::uint32_t>(limit));
	auto response = request(Command::read_records, writer.take(), 10000);
	ByteReader reader(response.payload);
	require_ok(reader);
	std::vector<mnc::meter_stream::MeterStreamRecord> records;
	const auto count = reader.u32();
	records.reserve(count);
	for (std::uint32_t i = 0; i < count; ++i)
		records.push_back(decode_record(reader));
	reader.require_finished();
	return records;
}

void MeterRecordStreamClient::acknowledge(
	std::string_view name, std::uint64_t cursor)
{
	ByteWriter writer;
	write_string(writer, name);
	writer.u64(cursor);
	auto response = request(Command::acknowledge_records, writer.take());
	ByteReader reader(response.payload);
	require_ok(reader);
	reader.require_finished();
}

mnc::meter_stream::StreamStatus MeterRecordStreamClient::status() const
{
	auto response = request(Command::get_stream_status);
	ByteReader reader(response.payload);
	require_ok(reader);
	mnc::meter_stream::StreamStatus result;
	result.durability = reader.u8() != 0;
	(void)reader.u8(); (void)reader.u16();
	result.oldest_cursor = reader.u64();
	result.newest_cursor = reader.u64();
	result.record_count = reader.u64();
	result.storage_bytes = reader.u64();
	result.session_start_cursor = reader.u64();
	result.dropped_unacknowledged_records = reader.u64();
	const auto count = reader.u32();
	for (std::uint32_t index = 0; index < count; ++index)
		result.consumers.push_back({read_string(reader), reader.u64()});
	reader.require_finished();
	return result;
}

DatabaseStoragePolicy MeterRecordStreamClient::policy() const
{
	auto response = request(Command::get_storage_policy);
	ByteReader reader(response.payload);
	require_ok(reader);
	auto result = decode_policy(reader);
	reader.require_finished();
	return result;
}

void MeterRecordStreamClient::apply_policy(DatabaseStoragePolicy policy)
{
	ByteWriter writer;
	encode_policy(writer, policy);
	auto response = request(Command::apply_storage_policy, writer.take(), 30000);
	ByteReader reader(response.payload);
	require_ok(reader);
	reader.require_finished();
}

std::optional<EnergyValues> MeterRecordStreamClient::energy() const
{
	auto response = request(Command::get_energy_snapshot);
	ByteReader reader(response.payload);
	require_ok(reader);
	const bool present = reader.u8() != 0;
	if (reader.u8() != 0 || reader.u16() != 0)
		throw std::runtime_error("invalid ENERGY snapshot presence header");
	if (!present) {
		reader.require_finished();
		return std::nullopt;
	}
	auto result = decode_energy_values(reader);
	reader.require_finished();
	return result;
}

std::optional<DemandValues> MeterRecordStreamClient::demand() const
{
	auto response = request(Command::get_demand_snapshot);
	ByteReader reader(response.payload);
	require_ok(reader);
	const bool present = reader.u8() != 0;
	if (reader.u8() != 0 || reader.u16() != 0)
		throw std::runtime_error("invalid DEMAND snapshot presence header");
	if (!present) {
		reader.require_finished();
		return std::nullopt;
	}
	auto result = decode_demand_values(reader);
	reader.require_finished();
	return result;
}

namespace {

std::vector<std::byte> encode_reset_request(
	const energy_ledger::ResetRequest &reset)
{
	ByteWriter writer;
	writer.u64(reset.expected_epoch);
	writer.i64(reset.requested_at_nanoseconds);
	write_string(writer, reset.idempotency_key);
	write_string(writer, reset.actor);
	write_string(writer, reset.request_id);
	return writer.take();
}

energy_ledger::ResetResult decode_reset_result(ByteReader &reader)
{
	energy_ledger::ResetResult result;
	result.epoch = reader.u64();
	result.replayed = reader.u8() != 0;
	if (reader.u8() != 0 || reader.u16() != 0)
		throw std::runtime_error("invalid reset response reserved field");
	return result;
}

} // namespace

energy_ledger::ResetResult MeterRecordStreamClient::reset_energy(
	const energy_ledger::ResetRequest &reset)
{
	auto response = request(Command::reset_energy,
		encode_reset_request(reset), 10000);
	ByteReader reader(response.payload);
	require_ok(reader);
	auto result = decode_reset_result(reader);
	reader.require_finished();
	return result;
}

energy_ledger::ResetResult MeterRecordStreamClient::reset_demand_peaks(
	const energy_ledger::ResetRequest &reset)
{
	auto response = request(Command::reset_demand_peaks,
		encode_reset_request(reset), 10000);
	ByteReader reader(response.payload);
	require_ok(reader);
	auto result = decode_reset_result(reader);
	reader.require_finished();
	return result;
}

} // namespace msap1::meter_stream
