#pragma once

#include "msap1/meter/history/historian_ipc.hpp"
#include "msap1/meter/MeterDataProvider/stream/meter_stream_ipc.hpp"

#include <span>

namespace msap1::web {

/** Typed application boundary for stream and historian service IPC. */
class DatabaseGateway final {
public:
	[[nodiscard]] mnc::meter_stream::StreamStatus stream_status() const;
	[[nodiscard]] history::HistorianStatus historian_status() const;
	[[nodiscard]] history::HistorianCapabilities
	historian_capabilities() const;
	[[nodiscard]] std::vector<history::HistoryPoint>
	query(const history::HistoryQuery &query) const;
	[[nodiscard]] std::vector<history::PowerQualityEventCatalogEntry>
	query_power_quality_events(
		const history::PowerQualityEventQuery &query = {}) const;
	[[nodiscard]] std::uint64_t delete_power_quality_events(
		std::span<const PowerQualityEventUuid> event_uuids) const;
	[[nodiscard]] std::uint64_t clear_power_quality_events() const;
	void clear_history(
		std::span<const mnc::meter_stream::DatabaseDataset> datasets) const;
	void recreate_history_database() const;
	[[nodiscard]] std::optional<EnergyValues> energy() const;
	[[nodiscard]] std::optional<DemandValues> demand() const;
	[[nodiscard]] energy_ledger::ResetResult reset_energy(
		const energy_ledger::ResetRequest &request) const;
	[[nodiscard]] energy_ledger::ResetResult reset_demand_peaks(
		const energy_ledger::ResetRequest &request) const;

private:
	mutable meter_stream::MeterRecordStreamClient stream_;
	mutable history::ipc::HistorianClient historian_;
};

} // namespace msap1::web
