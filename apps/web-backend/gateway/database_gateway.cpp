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

} // namespace msap1::web
