#include "meter_stream_service.hpp"

#include "msap1/meter/MeterDataProvider/stream/meter_stream_ipc.hpp"
#include "msap1/settings/settings_ipc.hpp"

#include <chrono>
#include <algorithm>
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
			const auto cursor = spool_->publish(record);
			report_dropped_records();
			output.u32(static_cast<std::uint32_t>(Status::ok)); output.u64(cursor);
			publish_event(Event::record_committed, cursor);
			break;
		}
		case Command::publish_records: {
			auto records = decode_records(input); input.require_finished();
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
		default: throw std::invalid_argument("unsupported meter-stream command");
		}
		connection->post_send(response(frame, std::move(output)));
	} catch (const std::exception &error) {
		ByteWriter failure;
		failure.u32(static_cast<std::uint32_t>(Status::storage_error));
		write_string(failure, error.what());
		connection->post_send(response(frame, std::move(failure),
			mnc::ipc::FrameKind::error));
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
