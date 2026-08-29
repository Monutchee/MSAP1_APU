#pragma once

#include "mnc/MeterDataProvider/stream/durable_meter_spool.hpp"
#include "mnc/ipc/ipc.hpp"
#include "mnc/service/service.hpp"
#include "msap1/meter/MeterDataProvider/stream/meter_stream_ipc.hpp"
#include "msap1/meter/energy_ledger.hpp"

#include <boost/asio/io_context.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

namespace msap1::meter_stream::daemon {

class MeterStreamService final : public mnc::Service {
public:
	MeterStreamService();

protected:
	void on_start() override;
	void on_reload() override;
	void on_stop() noexcept override;
	[[nodiscard]] mnc::ServiceHealth health() const override;

private:
	void handle(mnc::ipc::UnixStreamServer::Connection connection,
		mnc::ipc::Frame frame);
	void subscribe(const mnc::ipc::UnixStreamServer::Connection &connection);
	void publish_event(msap1::meter_stream::Event event,
		std::uint64_t cursor = 0);
	void report_dropped_records();
	boost::asio::io_context context_;
	std::unique_ptr<mnc::meter_stream::DurableMeterSpool> spool_;
	std::unique_ptr<msap1::energy_ledger::EnergyLedger> energy_ledger_;
	mnc::ipc::UnixStreamServer server_;
	std::thread worker_;
	std::vector<std::weak_ptr<mnc::ipc::FramedConnection>> subscribers_;
	std::atomic<bool> failed_{false};
	/* Hard-cap eviction reporting: the counter delta says how many records
	 * were lost, the time floor keeps a wedged consumer from flooding the
	 * journal at the publish rate. */
	std::uint64_t reported_dropped_records_ = 0;
	std::chrono::steady_clock::time_point last_drop_report_{};
};

} // namespace msap1::meter_stream::daemon
