#pragma once

#include "msap1/meter/history/historian_ipc.hpp"
#include "msap1/meter/stream/meter_stream_ipc.hpp"

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

private:
	mutable meter_stream::MeterStreamClient stream_;
	mutable history::ipc::HistorianClient historian_;
};

} // namespace msap1::web
