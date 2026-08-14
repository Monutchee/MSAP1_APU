#pragma once

#include "mnc/mqtt/paho_mqtt_client.hpp"
#include "mnc/service/service.hpp"
#include "msap1/meter/MeterDataProvider/snapshot/acquisition_meter_snapshot_provider.hpp"
#include "msap1/mqtt/meter_publication_scheduler.hpp"
#include "msap1/mqtt/mqtt_ipc.hpp"
#include "msap1/settings/definition/mqtt_settings.hpp"

#include <boost/asio/io_context.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace msap1::mqtt::daemon {

class MqttPublisherService final : public mnc::Service {
public:
	MqttPublisherService();

protected:
	void on_start() override;
	void on_reload() override;
	void on_stop() noexcept override;
	[[nodiscard]] mnc::ServiceHealth health() const override;

private:
	void apply(const msap1::settings::MqttSettings &settings);
	void stop_runtime() noexcept;
	void watch_settings();
	void try_connect();
	void materialize_tls_assets(const std::map<std::string, std::string> &assets);
	void handle_status(mnc::ipc::UnixStreamServer::Connection connection,
		mnc::ipc::Frame frame);
	[[nodiscard]] MqttServiceStatus status_snapshot() const;

	boost::asio::io_context context_;
	msap1::meter::AcquisitionMeterSnapshotProvider provider_;
	mnc::mqtt::PahoMqttClient client_;
	MeterPublicationScheduler scheduler_;
	mnc::ipc::UnixStreamServer status_server_;
	std::thread io_worker_;
	std::thread settings_watcher_;
	mutable std::mutex runtime_mutex_;
	mutable std::mutex settings_mutex_;
	msap1::settings::MqttSettings active_settings_;
	msap1::settings::MqttSettings observed_settings_;
	std::map<std::string, std::string> observed_secrets_;
	std::map<std::string, std::string> observed_assets_;
	std::atomic<bool> stopping_{false};
	std::atomic<bool> failed_{false};
	std::chrono::seconds reconnect_delay_{1};
	std::chrono::steady_clock::time_point next_connect_attempt_{};
	std::filesystem::path runtime_tls_root_{"/run/monutchee/mqtt-publisher"};
};

} // namespace msap1::mqtt::daemon
