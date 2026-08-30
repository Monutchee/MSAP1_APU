#pragma once

#include "mnc/ipc/ipc.hpp"
#include "msap1/meter/history/meter_history.hpp"

#include <string>
#include <memory>
#include <span>
#include <string_view>

namespace msap1::history::ipc {

inline constexpr std::string_view socket_path =
	"/run/monutchee/meter-historian.sock";

/* A bounded 50,000-point history response is larger than the generic IPC
 * limit. This service-specific limit still caps memory while permitting the
 * advertised query capability. */
inline constexpr mnc::ipc::ConnectionLimits connection_limits{
	.max_payload = 4u * 1024u * 1024u,
	.max_queued_frames = 128,
	.max_queued_bytes = 8u * 1024u * 1024u,
};

enum class Command : std::uint32_t {
	query_history = 1,
	get_capabilities,
	get_historian_status,
	get_storage_policy,
	apply_storage_policy,
	subscribe_historian_events,
	clear_datasets,
	recreate_database,
	query_power_quality_events,
	link_power_quality_event_waveform,
	delete_power_quality_events,
};

/** Server-pushed notifications emitted after a subscription is accepted. */
enum class Event : std::uint32_t {
	record_committed = 1,
	migration_started,
	migration_completed,
	migration_failed,
	maintenance_started,
	maintenance_completed,
	maintenance_failed,
};

class HistorianClient final {
public:
	explicit HistorianClient(std::string path = std::string(socket_path));
	[[nodiscard]] std::vector<HistoryPoint> query(const HistoryQuery &query) const;
	[[nodiscard]] std::vector<PowerQualityEventCatalogEntry>
	query_power_quality_events(
		const PowerQualityEventQuery &query = {}) const;
	[[nodiscard]] std::uint64_t delete_power_quality_events(
		std::span<const PowerQualityEventUuid> event_uuids) const;
	[[nodiscard]] std::uint64_t clear_power_quality_events() const;
	void link_power_quality_event_waveform(
		const PowerQualityEventUuid &event_uuid,
		const WaveformCaptureUuid &capture_uuid) const;
	[[nodiscard]] HistorianStatus status() const;
	[[nodiscard]] HistorianCapabilities capabilities() const;
	[[nodiscard]] std::vector<mnc::meter_stream::DatabaseStoragePolicy>
	policies() const;
	void apply_policies(
		const std::vector<mnc::meter_stream::DatabaseStoragePolicy> &policies) const;
	void clear_datasets(
		std::span<const mnc::meter_stream::DatabaseDataset> datasets) const;
	void recreate_database() const;

private:
	[[nodiscard]] mnc::ipc::Frame request(Command command,
		std::vector<std::byte> payload = {}, int timeout_ms = 3000) const;
	std::unique_ptr<mnc::ipc::PersistentBlockingClient> transport_;
};

[[nodiscard]] std::vector<std::byte> encode_query(const HistoryQuery &query);
[[nodiscard]] HistoryQuery decode_query(mnc::ipc::ByteReader &reader);
[[nodiscard]] std::vector<std::byte> encode_power_quality_event_query(
	const PowerQualityEventQuery &query);
[[nodiscard]] PowerQualityEventQuery decode_power_quality_event_query(
	mnc::ipc::ByteReader &reader);
void encode_power_quality_event_entry(mnc::ipc::ByteWriter &writer,
	const PowerQualityEventCatalogEntry &entry);
[[nodiscard]] PowerQualityEventCatalogEntry decode_power_quality_event_entry(
	mnc::ipc::ByteReader &reader);

} // namespace msap1::history::ipc
