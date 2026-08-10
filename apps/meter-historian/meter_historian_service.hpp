#pragma once

#include "mnc/ipc/ipc.hpp"
#include "mnc/service/service.hpp"
#include "msap1/meter/history/historian_ipc.hpp"
#include "msap1/meter/history/meter_history.hpp"
#include "msap1/meter/stream/meter_stream_ipc.hpp"

#include <boost/asio/io_context.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace msap1::history::daemon {

/**
 * Durable stream consumer that materializes period-specific historical
 * projections. The stream cursor is acknowledged only after the decoded
 * block and all of its values commit successfully.
 */
class MeterHistorianService final : public mnc::Service {
public:
	MeterHistorianService();

protected:
	void on_start() override;
	void on_reload() override;
	void on_stop() noexcept override;
	[[nodiscard]] mnc::ServiceHealth health() const override;

private:
	void consume();
	void ingest(const mnc::meter_stream::MeterStreamRecord &record);
	void backfill();
	void handle(mnc::ipc::UnixStreamServer::Connection connection,
		mnc::ipc::Frame frame);
	void subscribe(const mnc::ipc::UnixStreamServer::Connection &connection);
	void post_event(ipc::Event event, std::uint64_t cursor = 0);
	void publish_event(ipc::Event event, std::uint64_t cursor);

	boost::asio::io_context context_;
	msap1::meter_stream::MeterStreamClient stream_;
	std::unique_ptr<MeterHistoryStore> store_;
	mnc::ipc::UnixStreamServer server_;
	std::thread io_worker_;
	std::thread consumer_;
	std::atomic<bool> stopping_{false};
	std::atomic<bool> failed_{false};
	std::atomic<bool> consumer_healthy_{true};
	std::atomic<bool> migrating_{false};
	std::atomic<bool> backfill_incomplete_{false};
	std::atomic<std::uint64_t> oldest_available_stream_cursor_{0};
	std::mutex migration_mutex_;
	std::vector<std::weak_ptr<mnc::ipc::FramedConnection>> subscribers_;
};

} // namespace msap1::history::daemon
