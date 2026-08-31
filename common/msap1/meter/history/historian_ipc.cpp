#include "msap1/meter/history/historian_ipc.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace msap1::history::ipc {
namespace {
using mnc::ipc::ByteReader; using mnc::ipc::ByteWriter;

template <typename Uuid>
void write_uuid(ByteWriter &writer, const Uuid &uuid)
{
	writer.bytes(std::span<const std::byte>{uuid});
}

template <typename Uuid>
Uuid read_uuid(ByteReader &reader)
{
	Uuid uuid{};
	const auto bytes = reader.bytes(uuid.size());
	std::copy(bytes.begin(), bytes.end(), uuid.begin());
	return uuid;
}

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

std::vector<std::byte> encode_power_quality_event_query(
	const PowerQualityEventQuery &query)
{
	if (query.id && query.event_uuid)
		throw std::invalid_argument(
			"PQ event query cannot combine private ID and UUID");
	ByteWriter writer;
	std::uint8_t flags = 0u;
	flags |= query.id ? 1u << 0u : 0u;
	flags |= query.event_uuid ? 1u << 1u : 0u;
	flags |= query.start_utc_nanoseconds ? 1u << 2u : 0u;
	flags |= query.end_utc_nanoseconds ? 1u << 3u : 0u;
	writer.u8(flags);
	writer.u8(0u);
	writer.u16(0u);
	writer.u32(query.limit);
	if (query.id) {
		writer.u64(query.id->session);
		writer.u64(query.id->counter);
	}
	if (query.event_uuid)
		write_uuid(writer, *query.event_uuid);
	if (query.start_utc_nanoseconds)
		writer.i64(*query.start_utc_nanoseconds);
	if (query.end_utc_nanoseconds)
		writer.i64(*query.end_utc_nanoseconds);
	return writer.take();
}

PowerQualityEventQuery decode_power_quality_event_query(ByteReader &reader)
{
	PowerQualityEventQuery result;
	const auto flags = reader.u8();
	if ((flags & 0xf0u) != 0u || reader.u8() != 0u || reader.u16() != 0u)
		throw std::invalid_argument("PQ event query reserved fields are nonzero");
	result.limit = reader.u32();
	if ((flags & (1u << 0u)) != 0u)
		result.id = PowerQualityEventId{reader.u64(), reader.u64()};
	if ((flags & (1u << 1u)) != 0u)
		result.event_uuid = read_uuid<PowerQualityEventUuid>(reader);
	if ((flags & (1u << 2u)) != 0u)
		result.start_utc_nanoseconds = reader.i64();
	if ((flags & (1u << 3u)) != 0u)
		result.end_utc_nanoseconds = reader.i64();
	if (result.id && result.event_uuid)
		throw std::invalid_argument(
			"PQ event query combines private ID and UUID");
	return result;
}

void encode_power_quality_event_entry(ByteWriter &writer,
	const PowerQualityEventCatalogEntry &entry)
{
	const auto &event = entry.event;
	writer.u16(1u);
	std::uint16_t optional = 0u;
	optional |= entry.start_utc_nanoseconds ? 1u << 0u : 0u;
	optional |= entry.last_utc_nanoseconds ? 1u << 1u : 0u;
	optional |= entry.utc_uncertainty_nanoseconds ? 1u << 2u : 0u;
	writer.u16(optional);
	write_uuid(writer, entry.event_uuid);
	writer.u64(event.id.session);
	writer.u64(event.id.counter);
	writer.u8(static_cast<std::uint8_t>(event.lifecycle));
	writer.u8(static_cast<std::uint8_t>(event.type));
	writer.u8(event.phase_mask);
	writer.u8(event.trigger_source);
	writer.u32(event.sequence);
	writer.u32(event.configuration_generation);
	writer.u32(event.profile_generation);
	writer.u32(event.sample_rate_hz);
	writer.u64(event.first_sample);
	writer.u64(event.last_sample);
	writer.u64(event.trigger_sample);
	writer.u64(event.duration_samples);
	writer.u32(event.valid_mask);
	writer.u32(event.status);
	writer.u32(event.threshold_e4);
	writer.u32(event.hysteresis_e4);
	writer.u32(event.reference_micro_units);
	for (const auto value : event.minimum_micro_units) writer.u32(value);
	for (const auto value : event.maximum_micro_units) writer.u32(value);
	for (const auto value : event.current_micro_units) writer.u32(value);
	std::uint8_t event_flags = 0u;
	event_flags |= event.waveform_enabled ? 1u << 0u : 0u;
	event_flags |= event.per_phase ? 1u << 1u : 0u;
	event_flags |= event.iec_classification ? 1u << 2u : 0u;
	writer.u8(event_flags);
	writer.u8(static_cast<std::uint8_t>(event.time_quality));
	writer.u16(0u);
	writer.u32(event.waveform_pretrigger_ms);
	writer.u32(event.waveform_posttrigger_ms);
	writer.u32(event.waveform_decimation);
	writer.u64(event.start_utc_nanoseconds);
	writer.u64(event.last_utc_nanoseconds);
	writer.u32(event.discontinuities);
	writer.u32(event.update_count);
	for (const auto value : event.settings_digest) writer.u32(value);
	writer.u64(entry.stream_cursor);
	writer.i64(entry.start_utc_nanoseconds.value_or(0));
	writer.i64(entry.last_utc_nanoseconds.value_or(0));
	writer.u64(entry.utc_uncertainty_nanoseconds.value_or(0));
	if (entry.waveform_capture_uuids.size() > 4096u)
		throw std::invalid_argument("PQ event has too many waveform links");
	writer.u32(static_cast<std::uint32_t>(
		entry.waveform_capture_uuids.size()));
	for (const auto &uuid : entry.waveform_capture_uuids)
		write_uuid(writer, uuid);
}

PowerQualityEventCatalogEntry decode_power_quality_event_entry(
	ByteReader &reader)
{
	if (reader.u16() != 1u)
		throw std::invalid_argument("unsupported PQ event entry version");
	const auto optional = reader.u16();
	if ((optional & ~0x7u) != 0u)
		throw std::invalid_argument("PQ event entry optional flags are invalid");
	PowerQualityEventCatalogEntry entry{};
	entry.event_uuid = read_uuid<PowerQualityEventUuid>(reader);
	auto &event = entry.event;
	event.id = {reader.u64(), reader.u64()};
	event.lifecycle = static_cast<PowerQualityEventLifecycle>(reader.u8());
	event.type = static_cast<PowerQualityLifecycleType>(reader.u8());
	event.phase_mask = reader.u8();
	event.trigger_source = reader.u8();
	event.sequence = reader.u32();
	event.configuration_generation = reader.u32();
	event.profile_generation = reader.u32();
	event.sample_rate_hz = reader.u32();
	event.first_sample = reader.u64();
	event.last_sample = reader.u64();
	event.trigger_sample = reader.u64();
	event.duration_samples = reader.u64();
	event.valid_mask = reader.u32();
	event.status = reader.u32();
	event.threshold_e4 = reader.u32();
	event.hysteresis_e4 = reader.u32();
	event.reference_micro_units = reader.u32();
	for (auto &value : event.minimum_micro_units) value = reader.u32();
	for (auto &value : event.maximum_micro_units) value = reader.u32();
	for (auto &value : event.current_micro_units) value = reader.u32();
	const auto event_flags = reader.u8();
	if ((event_flags & ~0x7u) != 0u)
		throw std::invalid_argument("PQ event entry flags are invalid");
	event.waveform_enabled = (event_flags & (1u << 0u)) != 0u;
	event.per_phase = (event_flags & (1u << 1u)) != 0u;
	event.iec_classification = (event_flags & (1u << 2u)) != 0u;
	event.time_quality = static_cast<TimeQuality>(reader.u8());
	if (reader.u16() != 0u)
		throw std::invalid_argument("PQ event entry reserved field is nonzero");
	event.waveform_pretrigger_ms = reader.u32();
	event.waveform_posttrigger_ms = reader.u32();
	event.waveform_decimation = reader.u32();
	event.start_utc_nanoseconds = reader.u64();
	event.last_utc_nanoseconds = reader.u64();
	event.discontinuities = reader.u32();
	event.update_count = reader.u32();
	for (auto &value : event.settings_digest) value = reader.u32();
	entry.stream_cursor = reader.u64();
	const auto start = reader.i64();
	const auto last = reader.i64();
	const auto uncertainty = reader.u64();
	if ((optional & (1u << 0u)) != 0u)
		entry.start_utc_nanoseconds = start;
	if ((optional & (1u << 1u)) != 0u)
		entry.last_utc_nanoseconds = last;
	if ((optional & (1u << 2u)) != 0u)
		entry.utc_uncertainty_nanoseconds = uncertainty;
	const auto count = reader.u32();
	if (count > 4096u)
		throw std::invalid_argument("PQ event waveform link count is excessive");
	entry.waveform_capture_uuids.reserve(count);
	for (std::uint32_t index = 0; index < count; ++index)
		entry.waveform_capture_uuids.push_back(
			read_uuid<WaveformCaptureUuid>(reader));
	return entry;
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
		point.quality=static_cast<MeasurementQuality>(reader.u8());
		const bool has_reset_epoch = reader.u8() != 0;
		point.value=reader.i64();
		const auto reset_epoch = reader.u64();
		if (has_reset_epoch)
			point.reset_epoch = reset_epoch;
		points.push_back(point);
	}
	reader.require_finished();
	return points;
}

std::vector<PowerQualityEventCatalogEntry>
HistorianClient::query_power_quality_events(
	const PowerQualityEventQuery &query) const
{
	auto response = request(Command::query_power_quality_events,
		encode_power_quality_event_query(query), 10000);
	ByteReader reader(response.payload);
	require_ok(reader);
	const auto count = reader.u32();
	if (count > 10000u)
		throw std::runtime_error("historian returned too many PQ events");
	std::vector<PowerQualityEventCatalogEntry> result;
	result.reserve(count);
	for (std::uint32_t index = 0; index < count; ++index)
		result.push_back(decode_power_quality_event_entry(reader));
	reader.require_finished();
	return result;
}

void HistorianClient::link_power_quality_event_waveform(
	const PowerQualityEventUuid &event_uuid,
	const WaveformCaptureUuid &capture_uuid) const
{
	ByteWriter writer;
	write_uuid(writer, event_uuid);
	write_uuid(writer, capture_uuid);
	auto response = request(Command::link_power_quality_event_waveform,
		writer.take());
	ByteReader reader(response.payload);
	require_ok(reader);
	reader.require_finished();
}

std::uint64_t HistorianClient::delete_power_quality_events(
	std::span<const PowerQualityEventUuid> event_uuids) const
{
	if (event_uuids.empty())
		throw std::invalid_argument(
			"power-quality event deletion is empty");
	if (event_uuids.size() > 1000u)
		throw std::invalid_argument(
			"at most 1000 power-quality events may be deleted at once");
	ByteWriter writer;
	writer.u8(0u);
	writer.u32(static_cast<std::uint32_t>(event_uuids.size()));
	for (const auto &uuid : event_uuids)
		write_uuid(writer, uuid);
	auto response = request(Command::delete_power_quality_events,
		writer.take(), 120000);
	ByteReader reader(response.payload);
	require_ok(reader);
	const auto deleted = reader.u64();
	reader.require_finished();
	return deleted;
}

std::uint64_t HistorianClient::clear_power_quality_events() const
{
	ByteWriter writer;
	writer.u8(1u);
	writer.u32(0u);
	auto response = request(Command::delete_power_quality_events,
		writer.take(), 120000);
	ByteReader reader(response.payload);
	require_ok(reader);
	const auto deleted = reader.u64();
	reader.require_finished();
	return deleted;
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
	result.power_quality_event_count=reader.u64();
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
