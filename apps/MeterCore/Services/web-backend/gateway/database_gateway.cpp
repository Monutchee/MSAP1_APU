#include "gateway/database_gateway.hpp"

namespace msap1::web {

mnc::meter_stream::StreamStatus DatabaseGateway::stream_status() const
{
	return stream_.status();
}

history::HistorianStatus DatabaseGateway::historian_status() const
{
	return historian_.status();
}

history::HistorianCapabilities
DatabaseGateway::historian_capabilities() const
{
	return historian_.capabilities();
}

std::vector<history::HistoryPoint>
DatabaseGateway::query(const history::HistoryQuery &query) const
{
	return historian_.query(query);
}

std::vector<history::PowerQualityEventCatalogEntry>
DatabaseGateway::query_power_quality_events(
	const history::PowerQualityEventQuery &query) const
{
	return historian_.query_power_quality_events(query);
}

void DatabaseGateway::clear_history(
	std::span<const mnc::meter_stream::DatabaseDataset> datasets) const
{
	historian_.clear_datasets(datasets);
}

void DatabaseGateway::recreate_history_database() const
{
	historian_.recreate_database();
}

std::optional<EnergyValues> DatabaseGateway::energy() const
{
	return stream_.energy();
}

std::optional<DemandValues> DatabaseGateway::demand() const
{
	return stream_.demand();
}

energy_ledger::ResetResult DatabaseGateway::reset_energy(
	const energy_ledger::ResetRequest &request) const
{
	return stream_.reset_energy(request);
}

energy_ledger::ResetResult DatabaseGateway::reset_demand_peaks(
	const energy_ledger::ResetRequest &request) const
{
	return stream_.reset_demand_peaks(request);
}

} // namespace msap1::web
