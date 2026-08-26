#include "msap1/meter/MeterDataProvider/stream/meter_stream_ipc.hpp"

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
	throw std::runtime_error(read_string(reader));
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

} // namespace msap1::meter_stream
