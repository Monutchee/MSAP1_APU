#pragma once

#include "mnc/MeterDataProvider/stream/meter_stream.hpp"
#include "mnc/ipc/ipc.hpp"
#include "msap1/meter/energy_ledger.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace msap1::meter_stream {

inline constexpr std::string_view socket_path =
	"/run/monutchee/meter-stream.sock";
/* Version 2 appended session_start_cursor and dropped_unacknowledged_records
 * to the get_stream_status reply. Version 3 assigns the record envelope's
 * reserved u16 to source_fragment for multi-record producer families.
 * Version 4 adds an atomic bounded publish_records request. Version 5 adds
 * authoritative energy/demand snapshots and audited reset transactions.
 * Version 6 revises the pre-production DEMAND-v1 snapshot with method,
 * window, update-cadence, and profile-generation metadata. The version is
 * not carried on the wire — all peers ship in one image and the decoder's
 * require_finished() turns any accidental mix into a loud error frame. */
inline constexpr std::uint32_t protocol_version = 6;
inline constexpr std::size_t maximum_publish_records = 256;

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
	publish_records,
	get_energy_snapshot,
	get_demand_snapshot,
	reset_energy,
	reset_demand_peaks,
};

enum class Status : std::uint32_t {
	ok = 0,
	invalid_request,
	storage_error,
	permission_denied,
	internal_error,
	unavailable,
	conflict,
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
[[nodiscard]] std::vector<std::byte> encode_records(
	std::span<const mnc::meter_stream::MeterStreamRecord> records);
[[nodiscard]] std::vector<mnc::meter_stream::MeterStreamRecord> decode_records(
	mnc::ipc::ByteReader &reader);

[[nodiscard]] std::vector<std::byte> encode_energy_values(
	const EnergyValues &values);
[[nodiscard]] EnergyValues decode_energy_values(mnc::ipc::ByteReader &reader);
[[nodiscard]] std::vector<std::byte> encode_demand_values(
	const DemandValues &values);
[[nodiscard]] DemandValues decode_demand_values(mnc::ipc::ByteReader &reader);

/** Optional product authority used by acquisition after its durability barrier. */
class EnergyAuthority {
public:
	virtual ~EnergyAuthority() = default;
	[[nodiscard]] virtual std::optional<EnergyValues> energy() const = 0;
	[[nodiscard]] virtual std::optional<DemandValues> demand() const = 0;
};

/** Typed synchronous adapter used by acquisition and historian. */
class MeterRecordStreamClient final
	: public mnc::meter_stream::MeterRecordPublisher,
	  public mnc::meter_stream::MeterStreamConsumer,
	  public EnergyAuthority {
public:
	explicit MeterRecordStreamClient(
		std::string path = std::string(socket_path));
	std::uint64_t publish(
		const mnc::meter_stream::MeterStreamRecord &record) override;
	std::vector<std::uint64_t> publish_records(
		std::span<const mnc::meter_stream::MeterStreamRecord> records) override;
	void register_consumer(std::string_view name) override;
	void unregister_consumer(std::string_view name) override;
	std::vector<mnc::meter_stream::MeterStreamRecord> read_after(
		std::string_view name, std::size_t limit) override;
	void acknowledge(std::string_view name, std::uint64_t cursor) override;
	[[nodiscard]] mnc::meter_stream::StreamStatus status() const;
	[[nodiscard]] mnc::meter_stream::DatabaseStoragePolicy policy() const;
	void apply_policy(mnc::meter_stream::DatabaseStoragePolicy policy);
	[[nodiscard]] std::optional<EnergyValues> energy() const override;
	[[nodiscard]] std::optional<DemandValues> demand() const override;
	[[nodiscard]] energy_ledger::ResetResult reset_energy(
		const energy_ledger::ResetRequest &reset);
	[[nodiscard]] energy_ledger::ResetResult reset_demand_peaks(
		const energy_ledger::ResetRequest &reset);

private:
	[[nodiscard]] mnc::ipc::Frame request(Command command,
		std::vector<std::byte> payload = {}, int timeout_ms = 5000) const;
	std::unique_ptr<mnc::ipc::PersistentBlockingClient> transport_;
};

} // namespace msap1::meter_stream
