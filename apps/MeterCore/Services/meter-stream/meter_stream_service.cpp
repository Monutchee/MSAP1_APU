#include "meter_stream_service.hpp"

#include "msap1/meter/MeterDataProvider/stream/meter_stream_ipc.hpp"
#include "msap1/meter/energy_demand.hpp"
#include "msap1/settings/settings_ipc.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>

#include <sys/stat.h>

namespace msap1::meter_stream::daemon {
namespace {

using mnc::ipc::ByteReader;
using mnc::ipc::ByteWriter;
using msap1::meter_stream::Command;
using msap1::meter_stream::Event;
using msap1::meter_stream::Status;

void write_string(ByteWriter &writer, std::string_view text)
{
	writer.u32(static_cast<std::uint32_t>(text.size()));
	writer.bytes(mnc::ipc::to_payload(text));
}

std::string read_string(ByteReader &reader)
{
	const auto bytes = reader.bytes(reader.u32());
	return std::string(mnc::ipc::payload_view(bytes));
}

void write_policy(ByteWriter &writer,
	const mnc::meter_stream::DatabaseStoragePolicy &policy)
{
	writer.u8(static_cast<std::uint8_t>(policy.dataset));
	writer.u8(static_cast<std::uint8_t>(policy.backend)); writer.u16(0);
	writer.u64(policy.retention.maximum_age
		? static_cast<std::uint64_t>(policy.retention.maximum_age->count()) : 0);
	writer.u64(policy.retention.maximum_bytes.value_or(0));
}

mnc::meter_stream::DatabaseStoragePolicy read_policy(ByteReader &reader)
{
	mnc::meter_stream::DatabaseStoragePolicy result;
	result.dataset = static_cast<mnc::meter_stream::DatabaseDataset>(reader.u8());
	result.backend = static_cast<mnc::meter_stream::StorageBackend>(reader.u8());
	(void)reader.u16();
	if (const auto age = reader.u64(); age)
		result.retention.maximum_age = std::chrono::seconds(age);
	if (const auto bytes = reader.u64(); bytes)
		result.retention.maximum_bytes = bytes;
	return result;
}

mnc::ipc::Frame response(const mnc::ipc::Frame &request, ByteWriter writer,
	mnc::ipc::FrameKind kind = mnc::ipc::FrameKind::response)
{
	return {.kind = kind, .message_type = request.message_type,
		.correlation_id = request.correlation_id, .payload = writer.take()};
}

msap1::MeterRecord wire_record(
	const mnc::meter_stream::MeterStreamRecord &stream_record)
{
	if (stream_record.payload.size() != sizeof(msap1::MeterRecord))
		throw std::invalid_argument(
			"meter-stream payload is not one complete meter record");
	msap1::MeterRecord result{};
	std::memcpy(&result, stream_record.payload.data(), sizeof result);
	if (result.record_format() != stream_record.record_format)
		throw std::invalid_argument(
			"meter-stream envelope and payload record formats differ");
	return result;
}

std::optional<msap1::EnergyValues> decode_energy_batch(
	std::span<const mnc::meter_stream::MeterStreamRecord> records)
{
	const auto energy_count = std::count_if(records.begin(), records.end(),
		[](const auto &record) {
			return record.record_format == msap1::meter_energy_format;
		});
	if (energy_count == 0)
		return std::nullopt;
	if (energy_count != 2 || records.size() != 2)
		throw std::invalid_argument(
			"ENERGY-v1 must be published as one exact two-record family");
	const auto summary = wire_record(records[0]);
	const auto quadrants = wire_record(records[1]);
	if (summary.energy_part() != msap1::meter_energy_part_summary ||
	    quadrants.energy_part() != msap1::meter_energy_part_quadrants)
		throw std::invalid_argument(
			"ENERGY-v1 parts must be ordered summary then quadrants");
	return msap1::decode_energy_family(summary, quadrants,
		msap1::SystemTime(std::chrono::nanoseconds(
			records[0].ingested_at_nanoseconds)));
}

std::optional<msap1::DemandValues> decode_demand_record(
	const mnc::meter_stream::MeterStreamRecord &record)
{
	if (record.record_format == msap1::meter_energy_format)
		throw std::invalid_argument(
			"ENERGY-v1 cannot be published one part at a time");
	if (record.record_format != msap1::meter_demand_format)
		return std::nullopt;
	auto update = msap1::decode_demand_meter_record(wire_record(record),
		msap1::SystemTime(std::chrono::nanoseconds(
			record.ingested_at_nanoseconds)));
	if (!update.demand)
		throw std::logic_error("DEMAND-v1 decoder omitted its typed values");
	return std::move(update.demand);
}

std::uint32_t source_sequence(
	const mnc::meter_stream::MeterStreamRecord &record)
{
	if (record.source_sequence > std::numeric_limits<std::uint32_t>::max())
		throw std::invalid_argument("M17 source sequence exceeds uint32");
	return static_cast<std::uint32_t>(record.source_sequence);
}

msap1::energy_ledger::ResetRequest read_reset(ByteReader &reader)
{
	msap1::energy_ledger::ResetRequest reset;
	reset.expected_epoch = reader.u64();
	reset.requested_at_nanoseconds = reader.i64();
	reset.idempotency_key = read_string(reader);
	reset.actor = read_string(reader);
	reset.request_id = read_string(reader);
	reader.require_finished();
	return reset;
}

void write_reset(ByteWriter &writer,
	const msap1::energy_ledger::ResetResult &reset)
{
	writer.u64(reset.epoch);
	writer.u8(reset.replayed);
	writer.u8(0);
	writer.u16(0);
}

void post_failure(const mnc::ipc::UnixStreamServer::Connection &connection,
	const mnc::ipc::Frame &frame, Status status, std::string_view message)
{
	ByteWriter failure;
	failure.u32(static_cast<std::uint32_t>(status));
	write_string(failure, message);
	connection->post_send(response(frame, std::move(failure),
		mnc::ipc::FrameKind::error));
}

} // namespace

MeterStreamService::MeterStreamService()
	: Service("MSAP1 durable meter stream", "meter-stream"),
	  server_(context_.get_executor(),
		std::string(msap1::meter_stream::socket_path),
		msap1::meter_stream::connection_limits)
{
}

void MeterStreamService::on_start()
{
	auto settings = msap1::settings::ipc::SettingsClient{}.active();
	spool_ = std::make_unique<mnc::meter_stream::DurableMeterSpool>(
		"/data/mnc/database/meter-stream/spool.sqlite3",
		settings.database.spool_policy());
	energy_ledger_ = std::make_unique<msap1::energy_ledger::EnergyLedger>(
		"/data/mnc/database/meter-stream/energy.sqlite3");
	server_.start([this](auto connection, auto frame) {
		handle(std::move(connection), std::move(frame));
	}, [this](const std::string &message) {
		(void)logger().write(mnc::logging::Priority::warning,
			"meter-stream IPC error: " + message, "ipc_error");
	});
	if (::chmod(msap1::meter_stream::socket_path.data(), 0660) != 0)
		throw std::runtime_error("cannot set meter-stream socket mode");
	worker_ = std::thread([this] {
		try { context_.run(); }
		catch (...) { failed_ = true; request_stop(); }
	});
	(void)logger().write(mnc::logging::Priority::notice,
		"durable meter stream is ready", "stream_ready");
}

void MeterStreamService::on_stop() noexcept
{
	server_.stop(); context_.stop();
	if (worker_.joinable()) worker_.join();
	energy_ledger_.reset();
	spool_.reset();
}

void MeterStreamService::on_reload()
{
	(void)logger().write(mnc::logging::Priority::notice,
		"meter-stream reload requested; settings are applied transactionally over IPC",
		"reload_requested");
}

mnc::ServiceHealth MeterStreamService::health() const
{
	return failed_ ? mnc::ServiceHealth{false, "stream worker failed"}
		: mnc::ServiceHealth{true, "durable stream ready"};
}

void MeterStreamService::report_dropped_records()
{
	const auto dropped = spool_->dropped_unacknowledged_records();
	if (dropped == reported_dropped_records_)
		return;
	const auto now = std::chrono::steady_clock::now();
	if (now - last_drop_report_ < std::chrono::seconds(10))
		return;
	(void)logger().write(mnc::logging::Priority::warning,
		"spool byte cap evicted " +
			std::to_string(dropped - reported_dropped_records_) +
			" unacknowledged records (" + std::to_string(dropped) +
			" total); a consumer is not keeping up",
		"spool_records_dropped");
	reported_dropped_records_ = dropped;
	last_drop_report_ = now;
}

void MeterStreamService::handle(mnc::ipc::UnixStreamServer::Connection connection,
	mnc::ipc::Frame frame)
{
	ByteWriter output;
	try {
		if (frame.kind != mnc::ipc::FrameKind::request)
			throw std::invalid_argument("meter-stream frame is not a request");
		ByteReader input(frame.payload);
		switch (static_cast<Command>(frame.message_type)) {
		case Command::publish_record: {
			auto record = decode_record(input); input.require_finished();
			auto demand = decode_demand_record(record);
			if (demand)
				(void)energy_ledger_->ingest_demand(*demand,
					source_sequence(record),
					record.configuration_generation,
					record.ingested_at_nanoseconds);
			/* FULL ledger commit is the M17 publication barrier. If the spool
			 * write then fails, retry is safe because ledger ingestion is
			 * idempotent for the same source sequence. */
			const auto cursor = spool_->publish(record);
			report_dropped_records();
			output.u32(static_cast<std::uint32_t>(Status::ok)); output.u64(cursor);
			publish_event(Event::record_committed, cursor);
			break;
		}
		case Command::publish_records: {
			auto records = decode_records(input); input.require_finished();
			if (std::any_of(records.begin(), records.end(), [](const auto &record) {
				return record.record_format == msap1::meter_demand_format;
			}))
				throw std::invalid_argument(
					"DEMAND-v1 must use the single-record publication command");
			auto energy = decode_energy_batch(records);
			if (energy)
				(void)energy_ledger_->ingest_energy(*energy,
					source_sequence(records.front()),
					records.front().configuration_generation,
					records.front().ingested_at_nanoseconds);
			/* Never expose or acknowledge a two-part ENERGY family until its
			 * authoritative transaction has reached durable storage. */
			const auto cursors = spool_->publish_records(records);
			report_dropped_records();
			output.u32(static_cast<std::uint32_t>(Status::ok));
			output.u32(static_cast<std::uint32_t>(cursors.size()));
			for (const auto cursor : cursors)
				output.u64(cursor);
			publish_event(Event::record_committed, cursors.back());
			break;
		}
		case Command::register_consumer:
			spool_->register_consumer(read_string(input)); input.require_finished();
			output.u32(static_cast<std::uint32_t>(Status::ok)); break;
		case Command::unregister_consumer:
			spool_->unregister_consumer(read_string(input)); input.require_finished();
			output.u32(static_cast<std::uint32_t>(Status::ok)); break;
		case Command::read_records: {
			const auto name = read_string(input); const auto limit = input.u32();
			input.require_finished();
			const auto records = spool_->read_after(name, limit);
			output.u32(static_cast<std::uint32_t>(Status::ok));
			output.u32(static_cast<std::uint32_t>(records.size()));
			for (const auto &record : records) output.bytes(encode_record(record));
			break;
		}
		case Command::acknowledge_records:
			spool_->acknowledge(read_string(input), input.u64()); input.require_finished();
			spool_->prune();
			output.u32(static_cast<std::uint32_t>(Status::ok));
			publish_event(Event::consumer_advanced); break;
		case Command::get_stream_status: {
			input.require_finished();
			const auto status = spool_->status();
			output.u32(static_cast<std::uint32_t>(Status::ok));
			output.u8(status.durability); output.u8(0); output.u16(0);
			output.u64(status.oldest_cursor); output.u64(status.newest_cursor);
			output.u64(status.record_count); output.u64(status.storage_bytes);
			output.u64(status.session_start_cursor);
			output.u64(status.dropped_unacknowledged_records);
			output.u32(static_cast<std::uint32_t>(status.consumers.size()));
			for (const auto &consumer : status.consumers) {
				write_string(output, consumer.name);
				output.u64(consumer.acknowledged_cursor);
			}
			break;
		}
		case Command::get_storage_policy:
			input.require_finished();
			output.u32(static_cast<std::uint32_t>(Status::ok));
			write_policy(output, spool_->policy()); break;
		case Command::apply_storage_policy:
			spool_->apply_policy(read_policy(input)); input.require_finished();
			output.u32(static_cast<std::uint32_t>(Status::ok));
			publish_event(Event::storage_policy_applied); break;
		case Command::subscribe_stream_events:
			input.require_finished();
			subscribe(connection);
			output.u32(static_cast<std::uint32_t>(Status::ok)); break;
		case Command::get_energy_snapshot: {
			input.require_finished();
			const auto snapshot = energy_ledger_->energy();
			output.u32(static_cast<std::uint32_t>(Status::ok));
			output.u8(snapshot.has_value()); output.u8(0); output.u16(0);
			if (snapshot)
				output.bytes(encode_energy_values(*snapshot));
			break;
		}
		case Command::get_demand_snapshot: {
			input.require_finished();
			const auto snapshot = energy_ledger_->demand();
			output.u32(static_cast<std::uint32_t>(Status::ok));
			output.u8(snapshot.has_value()); output.u8(0); output.u16(0);
			if (snapshot)
				output.bytes(encode_demand_values(*snapshot));
			break;
		}
		case Command::reset_energy: {
			const auto reset = energy_ledger_->reset_energy(read_reset(input));
			output.u32(static_cast<std::uint32_t>(Status::ok));
			write_reset(output, reset);
			break;
		}
		case Command::reset_demand_peaks: {
			const auto reset = energy_ledger_->reset_demand_peaks(read_reset(input));
			output.u32(static_cast<std::uint32_t>(Status::ok));
			write_reset(output, reset);
			break;
		}
		default: throw std::invalid_argument("unsupported meter-stream command");
		}
		connection->post_send(response(frame, std::move(output)));
	} catch (const msap1::energy_ledger::Unavailable &error) {
		post_failure(connection, frame, Status::unavailable, error.what());
	} catch (const msap1::energy_ledger::Conflict &error) {
		post_failure(connection, frame, Status::conflict, error.what());
	} catch (const std::invalid_argument &error) {
		post_failure(connection, frame, Status::invalid_request, error.what());
	} catch (const std::exception &error) {
		post_failure(connection, frame, Status::storage_error, error.what());
	}
}

void MeterStreamService::subscribe(
	const mnc::ipc::UnixStreamServer::Connection &connection)
{
	subscribers_.erase(std::remove_if(subscribers_.begin(), subscribers_.end(),
		[](const auto &candidate) { return candidate.expired(); }),
		subscribers_.end());
	subscribers_.push_back(connection);
}

void MeterStreamService::publish_event(Event event, std::uint64_t cursor)
{
	mnc::ipc::ByteWriter payload;
	payload.u32(static_cast<std::uint32_t>(event));
	payload.u64(cursor);
	for (auto iterator = subscribers_.begin(); iterator != subscribers_.end();) {
		if (auto connection = iterator->lock(); connection && connection->is_open()) {
			connection->post_send({.kind = mnc::ipc::FrameKind::event,
				.message_type = static_cast<std::uint32_t>(
					Command::subscribe_stream_events),
				.correlation_id = 0, .payload = payload.data()});
			++iterator;
		} else {
			iterator = subscribers_.erase(iterator);
		}
	}
}

} // namespace msap1::meter_stream::daemon
