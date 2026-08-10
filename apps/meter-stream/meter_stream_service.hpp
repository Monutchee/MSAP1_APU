#pragma once

#include "mnc/MeterDataStreamer/meter_stream.hpp"
#include "mnc/ipc/ipc.hpp"
#include "mnc/service/service.hpp"
#include "msap1/meter/stream/meter_stream_ipc.hpp"

#include <boost/asio/io_context.hpp>

#include <atomic>
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
	boost::asio::io_context context_;
	std::unique_ptr<mnc::meter_stream::DurableMeterSpool> spool_;
	mnc::ipc::UnixStreamServer server_;
	std::thread worker_;
	std::vector<std::weak_ptr<mnc::ipc::FramedConnection>> subscribers_;
	std::atomic<bool> failed_{false};
};

} // namespace msap1::meter_stream::daemon
