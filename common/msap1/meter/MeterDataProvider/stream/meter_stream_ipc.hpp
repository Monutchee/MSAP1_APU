#pragma once

#include "mnc/MeterDataProvider/stream/meter_stream.hpp"
#include "mnc/ipc/ipc.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace msap1::meter_stream {

inline constexpr std::string_view socket_path =
	"/run/monutchee/meter-stream.sock";
/* Version 2 appended session_start_cursor and dropped_unacknowledged_records
 * to the get_stream_status reply.  The version is not carried on the wire —
 * all peers ship in one image and the decoder's require_finished() turns any
 * accidental mix into a loud error frame. */
inline constexpr std::uint32_t protocol_version = 2;

/* A maximum ReadRecords reply can contain 4096 complete 256-byte PL records
 * plus timing metadata. Keep that bounded batch valid without relaxing the
 * product-neutral IPC library's conservative 1 MiB default. */
inline constexpr mnc::ipc::ConnectionLimits connection_limits{
	.max_payload = 4u * 1024u * 1024u,
	.max_queued_frames = 128,
	.max_queued_bytes = 8u * 1024u * 1024u,
};

enum class Command : std::uint32_t {
	publish_record = 1,
	register_consumer,
	unregister_consumer,
	read_records,
	acknowledge_records,
	get_stream_status,
	get_storage_policy,
	apply_storage_policy,
	subscribe_stream_events,
};

enum class Status : std::uint32_t {
	ok = 0,
	invalid_request,
	storage_error,
	permission_denied,
	internal_error,
};

/** Notifications delivered after SubscribeStreamEvents succeeds. */
enum class Event : std::uint32_t {
	record_committed = 1,
	consumer_advanced,
	storage_policy_applied,
};

[[nodiscard]] std::vector<std::byte> encode_record(
	const mnc::meter_stream::MeterStreamRecord &record);
[[nodiscard]] mnc::meter_stream::MeterStreamRecord decode_record(
	mnc::ipc::ByteReader &reader);

/** Typed synchronous adapter used by acquisition and historian. */
class MeterRecordStreamClient final
	: public mnc::meter_stream::MeterRecordPublisher,
	  public mnc::meter_stream::MeterStreamConsumer {
public:
	explicit MeterRecordStreamClient(
		std::string path = std::string(socket_path));
	std::uint64_t publish(
		const mnc::meter_stream::MeterStreamRecord &record) override;
	void register_consumer(std::string_view name) override;
	void unregister_consumer(std::string_view name) override;
	std::vector<mnc::meter_stream::MeterStreamRecord> read_after(
		std::string_view name, std::size_t limit) override;
	void acknowledge(std::string_view name, std::uint64_t cursor) override;
	[[nodiscard]] mnc::meter_stream::StreamStatus status() const;
	[[nodiscard]] mnc::meter_stream::DatabaseStoragePolicy policy() const;
	void apply_policy(mnc::meter_stream::DatabaseStoragePolicy policy);

private:
	[[nodiscard]] mnc::ipc::Frame request(Command command,
		std::vector<std::byte> payload = {}, int timeout_ms = 5000) const;
	std::unique_ptr<mnc::ipc::PersistentBlockingClient> transport_;
};

} // namespace msap1::meter_stream
