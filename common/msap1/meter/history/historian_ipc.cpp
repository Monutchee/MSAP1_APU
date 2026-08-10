#include "msap1/meter/history/historian_ipc.hpp"

#include <chrono>
#include <stdexcept>

namespace msap1::history::ipc {
namespace {
using mnc::ipc::ByteReader; using mnc::ipc::ByteWriter;
void require_ok(ByteReader &reader)
{
	if (reader.u32() != 0) {
		const auto bytes = reader.bytes(reader.u32());
		throw std::runtime_error(std::string(mnc::ipc::payload_view(bytes)));
	}
}

void encode_policy(ByteWriter &writer,
	const mnc::meter_stream::DatabaseStoragePolicy &policy)
{
	writer.u8(static_cast<std::uint8_t>(policy.dataset));
	writer.u8(static_cast<std::uint8_t>(policy.backend)); writer.u16(0);
	writer.u64(policy.retention.maximum_age
		? static_cast<std::uint64_t>(policy.retention.maximum_age->count()) : 0);
	writer.u64(policy.retention.maximum_bytes.value_or(0));
}

mnc::meter_stream::DatabaseStoragePolicy decode_policy(ByteReader &reader)
{
	mnc::meter_stream::DatabaseStoragePolicy policy;
	policy.dataset = static_cast<mnc::meter_stream::DatabaseDataset>(reader.u8());
	policy.backend = static_cast<mnc::meter_stream::StorageBackend>(reader.u8());
	(void)reader.u16();
	if (const auto age = reader.u64(); age != 0)
		policy.retention.maximum_age = std::chrono::seconds(age);
	if (const auto bytes = reader.u64(); bytes != 0)
		policy.retention.maximum_bytes = bytes;
	return policy;
}
}

std::vector<std::byte> encode_query(const HistoryQuery &query)
{
	ByteWriter writer; writer.u8(static_cast<std::uint8_t>(query.period));
	writer.u8(0); writer.u16(static_cast<std::uint16_t>(query.attributes.size()));
	writer.i64(query.start_nanoseconds); writer.i64(query.end_nanoseconds);
	writer.u32(query.limit);
	for (auto id : query.attributes) writer.u16(static_cast<std::uint16_t>(id));
	return writer.take();
}

HistoryQuery decode_query(ByteReader &reader)
{
	HistoryQuery result; result.period = static_cast<MeasurementPeriod>(reader.u8());
	(void)reader.u8(); const auto count = reader.u16(); result.start_nanoseconds = reader.i64();
	result.end_nanoseconds = reader.i64(); result.limit = reader.u32();
	for (std::uint16_t i = 0; i < count; ++i)
		result.attributes.push_back(static_cast<mnc::meter::MeterAttributeId>(reader.u16()));
	return result;
}

HistorianClient::HistorianClient(std::string path)
	: transport_(std::make_unique<mnc::ipc::PersistentBlockingClient>(
		std::move(path), connection_limits))
{
}

mnc::ipc::Frame HistorianClient::request(Command command,
	std::vector<std::byte> payload, int timeout_ms) const
{
	mnc::ipc::Frame frame{
		.kind = mnc::ipc::FrameKind::request,
		.message_type = static_cast<std::uint32_t>(command),
		.payload = std::move(payload),
	};
	return transport_->request(std::move(frame), timeout_ms);
}

std::vector<HistoryPoint> HistorianClient::query(const HistoryQuery &query) const
{
	auto response = request(Command::query_history, encode_query(query), 10000);
	ByteReader reader(response.payload); require_ok(reader);
	std::vector<HistoryPoint> points; const auto count = reader.u32(); points.reserve(count);
	for (std::uint32_t i=0; i<count; ++i) {
		HistoryPoint point; point.measured_at_nanoseconds=reader.i64();
		point.source_sequence=reader.u64();
		point.attribute=static_cast<mnc::meter::MeterAttributeId>(reader.u16());
		point.quality=static_cast<MeasurementQuality>(reader.u8()); (void)reader.u8();
		point.value=reader.i64(); points.push_back(point);
	}
	reader.require_finished();
	return points;
}

HistorianStatus HistorianClient::status() const
{
	auto response = request(Command::get_historian_status);
	ByteReader reader(response.payload); require_ok(reader);
	HistorianStatus result; result.healthy=reader.u8()!=0;
	result.migration_in_progress=reader.u8()!=0;
	result.backfill_incomplete=reader.u8()!=0; (void)reader.u8();
	result.acknowledged_cursor=reader.u64();
	result.oldest_available_stream_cursor=reader.u64();
	result.block_count=reader.u64();
	result.storage_bytes=reader.u64();
	const auto count = reader.u32();
	for (std::uint32_t index = 0; index < count; ++index) {
		HistorianStatus::DatasetStatus item;
		item.dataset = static_cast<mnc::meter_stream::DatabaseDataset>(reader.u8());
		item.backend = static_cast<mnc::meter_stream::StorageBackend>(reader.u8());
		const bool has_oldest = reader.u8() != 0;
		const bool has_newest = reader.u8() != 0;
		item.block_count = reader.u64(); item.storage_bytes = reader.u64();
		const auto oldest = reader.i64(); const auto newest = reader.i64();
		if (has_oldest) item.oldest_nanoseconds = oldest;
		if (has_newest) item.newest_nanoseconds = newest;
		result.datasets.push_back(item);
	}
	reader.require_finished();
	return result;
}

HistorianCapabilities HistorianClient::capabilities() const
{
	auto response = request(Command::get_capabilities);
	ByteReader reader(response.payload);
	require_ok(reader);
	HistorianCapabilities result;
	const auto period_count = reader.u32();
	result.periods.reserve(period_count);
	for (std::uint32_t index = 0; index < period_count; ++index)
		result.periods.push_back(
			static_cast<MeasurementPeriod>(reader.u8()));
	const auto attribute_count = reader.u32();
	result.attributes.reserve(attribute_count);
	for (std::uint32_t index = 0; index < attribute_count; ++index)
		result.attributes.push_back(
			static_cast<mnc::meter::MeterAttributeId>(reader.u16()));
	result.maximum_points = reader.u32();
	reader.require_finished();
	return result;
}

std::vector<mnc::meter_stream::DatabaseStoragePolicy>
HistorianClient::policies() const
{
	auto response = request(Command::get_storage_policy);
	ByteReader reader(response.payload); require_ok(reader);
	std::vector<mnc::meter_stream::DatabaseStoragePolicy> result;
	const auto count = reader.u32(); result.reserve(count);
	for (std::uint32_t index = 0; index < count; ++index)
		result.push_back(decode_policy(reader));
	reader.require_finished();
	return result;
}

void HistorianClient::apply_policies(
	const std::vector<mnc::meter_stream::DatabaseStoragePolicy> &policies) const
{
	ByteWriter writer; writer.u32(static_cast<std::uint32_t>(policies.size()));
	for (const auto &policy : policies) encode_policy(writer, policy);
	auto response = request(Command::apply_storage_policy,
		writer.take(), 120000);
	ByteReader reader(response.payload); require_ok(reader);
	reader.require_finished();
}

void HistorianClient::clear_datasets(
	std::span<const mnc::meter_stream::DatabaseDataset> datasets) const
{
	ByteWriter writer;
	writer.u32(static_cast<std::uint32_t>(datasets.size()));
	for (const auto dataset : datasets)
		writer.u8(static_cast<std::uint8_t>(dataset));
	auto response = request(Command::clear_datasets, writer.take(), 120000);
	ByteReader reader(response.payload);
	require_ok(reader);
	reader.require_finished();
}

void HistorianClient::recreate_database() const
{
	auto response = request(Command::recreate_database, {}, 120000);
	ByteReader reader(response.payload);
	require_ok(reader);
	reader.require_finished();
}

} // namespace msap1::history::ipc
