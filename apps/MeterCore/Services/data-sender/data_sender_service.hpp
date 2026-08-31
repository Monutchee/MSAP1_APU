#pragma once

#include "mnc/datalogger/scheduler.hpp"
#include "mnc/datalogger/transfer.hpp"
#include "mnc/service/service.hpp"
#include "msap1/datalogger/data_sender_ipc.hpp"
#include "msap1/datalogger/msap1_datalogger.hpp"
#include "msap1/settings/settings.hpp"

#include <boost/asio/io_context.hpp>

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

namespace msap1::datalogger::daemon {

class DataSenderService final : public mnc::Service {
public:
	DataSenderService();

protected:
	void on_start() override;
	void on_reload() override;
	void on_stop() noexcept override;
	[[nodiscard]] mnc::ServiceHealth health() const override;

private:
	using MaterialByChannel =
		std::map<std::string, std::map<std::string, std::string>>;
	struct ConfigurationBundle {
		msap1::settings::ProductSettings settings;
		MaterialByChannel credentials;
		MaterialByChannel assets;
		bool operator==(const ConfigurationBundle &other) const
		{
			return credentials == other.credentials && assets == other.assets &&
				msap1::settings::SettingsCodec::encode(settings, false) ==
				msap1::settings::SettingsCodec::encode(
					other.settings, false);
		}
	};
	struct ChannelRuntime;

	[[nodiscard]] ConfigurationBundle load_configuration() const;
	void apply_configuration(ConfigurationBundle bundle);
	void materialize_assets(std::string_view channel_id,
		const std::map<std::string, std::string> &assets);
	void cleanup_assets(std::string_view channel_id) noexcept;
	void scheduler_loop();
	void delivery_loop();
	void settings_loop();
	[[nodiscard]] mnc::datalogger::DeliveryResult deliver(
		std::string_view channel_id,
		const mnc::datalogger::DeliveryRequest &request);
	void handle(mnc::ipc::UnixStreamServer::Connection connection,
		mnc::ipc::Frame frame);
	[[nodiscard]] ipc::ServiceStatus status_snapshot() const;
	[[nodiscard]] ipc::ChannelTestResult test_channel(std::string_view id);

	boost::asio::io_context context_;
	msap1::history::ipc::HistorianClient historian_;
	msap1::datalogger::Msap1Datalogger datalogger_;
	mnc::datalogger::DefaultMeterDataContentWriterFactory writers_;
	mnc::datalogger::SystemClock clock_;
	mnc::datalogger::SqliteOutboxRepository outbox_;
	mnc::datalogger::DataSenderEngine engine_;
	mnc::datalogger::CurlTransferClient transfer_;
	mnc::ipc::UnixStreamServer server_;

	mutable std::mutex configuration_mutex_;
	ConfigurationBundle active_bundle_;
	std::optional<ConfigurationBundle> pending_bundle_;
	std::unordered_map<std::string, std::shared_ptr<ChannelRuntime>> channels_;
	std::filesystem::path runtime_assets_root_{
		"/run/monutchee/data-sender"};

	std::thread io_worker_;
	std::thread scheduler_worker_;
	std::vector<std::thread> delivery_workers_;
	std::thread settings_worker_;
	mutable std::mutex wait_mutex_;
	std::condition_variable wait_condition_;
	std::atomic<bool> stopping_{false};
	std::atomic<bool> worker_failed_{false};
	mutable std::mutex failure_mutex_;
	std::string failure_message_;
};

} // namespace msap1::datalogger::daemon
