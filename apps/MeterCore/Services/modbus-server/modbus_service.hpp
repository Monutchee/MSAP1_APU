#pragma once

#include "mnc/modbus/modbus.hpp"
#include "mnc/service/service.hpp"
#include "msap1/meter/MeterDataProvider/snapshot/acquisition_meter_snapshot_provider.hpp"
#include "msap1/modbus/modbus_register_map.hpp"
#include "msap1/settings/definition/modbus_settings.hpp"

#include <boost/asio/io_context.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace msap1::modbus::daemon {

/**
 * Product composition root for Modbus/TCP and all configured RTU ports.
 *
 * Dependencies are injected once: the product-neutral protocol engine sees
 * only RegisterBank, while the MSAP1 adapter sees only MeterSnapshotProvider.
 * It therefore has no knowledge of acquisition IPC framing or DMA ownership.
 */
class ModbusService final : public mnc::Service {
public:
	ModbusService();

protected:
	void on_start() override;
	void on_reload() override;
	void on_stop() noexcept override;
	[[nodiscard]] mnc::ServiceHealth health() const override;

private:
	void start_transports(const msap1::settings::ModbusSettings &settings);
	void stop_transports() noexcept;
	void watch_settings();
	void report_transport_error(std::string_view transport,
		std::string_view message);

	boost::asio::io_context context_;
	msap1::meter::AcquisitionMeterSnapshotProvider meter_data_;
	msap1::modbus::Msap1RegisterBank register_bank_;
	mnc::modbus::RequestHandler request_handler_;
	std::unique_ptr<mnc::modbus::ModbusServer> server_;
	std::thread io_worker_;
	std::thread settings_watcher_;
	mutable std::mutex settings_mutex_;
	msap1::settings::ModbusSettings active_settings_;
	/* Last settings document seen by the watcher. This remains distinct from
	 * active_settings_: a rejected candidate must not trigger a destructive
	 * stop/rollback cycle every time the watcher polls. */
	msap1::settings::ModbusSettings observed_settings_;
	std::atomic<bool> stopping_{false};
	std::atomic<bool> failed_{false};
};

} // namespace msap1::modbus::daemon
