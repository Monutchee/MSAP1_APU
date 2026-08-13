#include "modbus_service.hpp"

#include "msap1/settings/settings_ipc.hpp"

#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace msap1::modbus::daemon {
namespace {

using namespace std::chrono_literals;

mnc::modbus::SerialParity parity(std::string_view value)
{
	if (value == "none")
		return mnc::modbus::SerialParity::none;
	if (value == "even")
		return mnc::modbus::SerialParity::even;
	if (value == "odd")
		return mnc::modbus::SerialParity::odd;
	throw std::invalid_argument("unsupported Modbus RTU parity");
}

} // namespace

ModbusService::ModbusService()
	: Service("MSAP1 Modbus server", "modbus"),
	  register_bank_(meter_data_), request_handler_(register_bank_)
{
}

void ModbusService::on_start()
{
	const auto settings = msap1::settings::ipc::SettingsClient{}.active();
	settings.modbus.validate();
	{
		std::scoped_lock lock(settings_mutex_);
		active_settings_ = settings.modbus;
	}
	start_transports(settings.modbus);
	settings_watcher_ = std::thread([this] { watch_settings(); });
	(void)logger().write(mnc::logging::Priority::notice,
		"Modbus service configuration is active", "modbus_ready");
}

void ModbusService::start_transports(
	const msap1::settings::ModbusSettings &settings)
{
	context_.restart();
	server_ = std::make_unique<mnc::modbus::ModbusServer>();
	if (settings.enabled && settings.tcp.enabled) {
		server_->add(std::make_unique<mnc::modbus::ModbusTcpServer>(
			context_.get_executor(), request_handler_,
			mnc::modbus::TcpServerConfig{
				.bind_address = settings.tcp.listen_address,
				.port = settings.tcp.port,
				.maximum_clients = settings.tcp.maximum_clients,
				.unit_id = settings.tcp.unit_id},
			[this](auto transport, auto message) {
				report_transport_error(transport, message);
			}));
	}

	std::vector<mnc::modbus::RtuPortConfig> rtu;
	if (settings.enabled) {
		for (const auto &port : settings.rtu) {
			if (!port.enabled)
				continue;
			rtu.push_back({.device = port.device,
				.baud_rate = port.baud_rate,
				.data_bits = port.data_bits,
				.parity = parity(port.parity),
				.stop_bits = port.stop_bits,
				.unit_id = port.unit_id});
		}
	}
	if (!rtu.empty()) {
		server_->add(std::make_unique<mnc::modbus::ModbusRtuServer>(
			context_.get_executor(), request_handler_, std::move(rtu),
			[this](auto transport, auto message) {
				report_transport_error(transport, message);
			}));
	}

	server_->start();
	if (!server_->empty()) {
		io_worker_ = std::thread([this] {
			try {
				context_.run();
			} catch (const std::exception &error) {
				(void)logger().write(mnc::logging::Priority::critical,
					"Modbus I/O worker failed: " + std::string(error.what()),
					"io_worker_failed");
				failed_ = true;
				request_stop();
			}
		});
	}
}

void ModbusService::stop_transports() noexcept
{
	if (server_)
		server_->stop();
	context_.stop();
	if (io_worker_.joinable())
		io_worker_.join();
	server_.reset();
}

void ModbusService::on_reload()
{
	const auto settings = msap1::settings::ipc::SettingsClient{}.active();
	settings.modbus.validate();
	{
		std::scoped_lock lock(settings_mutex_);
		if (settings.modbus == active_settings_)
			return;
	}
	stop_transports();
	try {
		start_transports(settings.modbus);
		{
			std::scoped_lock lock(settings_mutex_);
			active_settings_ = settings.modbus;
		}
		(void)logger().write(mnc::logging::Priority::notice,
			"Modbus transports reconfigured", "configuration_applied");
	} catch (const std::exception &error) {
		/* The persisted document is valid, but a bind/open may still fail.
		 * Restore the last working runtime configuration before reporting. */
		msap1::settings::ModbusSettings previous;
		{
			std::scoped_lock lock(settings_mutex_);
			previous = active_settings_;
		}
		stop_transports();
		try {
			start_transports(previous);
			(void)logger().write(mnc::logging::Priority::error,
				"Modbus reconfiguration rejected; restored the prior "
				"runtime configuration: " + std::string(error.what()),
				"configuration_rolled_back");
		} catch (const std::exception &rollback_error) {
			failed_ = true;
			throw std::runtime_error(
				"Modbus configuration and rollback both failed: " +
				std::string(error.what()) + "; rollback: " +
				rollback_error.what());
		}
	}
}

void ModbusService::watch_settings()
{
	while (!stopping_) {
		for (int tick = 0; tick < 10 && !stopping_; ++tick)
			std::this_thread::sleep_for(200ms);
		if (stopping_)
			break;
		try {
			const auto settings =
				msap1::settings::ipc::SettingsClient{}.active(1500);
			std::scoped_lock lock(settings_mutex_);
			if (settings.modbus != active_settings_)
				request_reload();
		} catch (const std::exception &error) {
			(void)logger().write(mnc::logging::Priority::debug,
				"Modbus settings check deferred: " +
					std::string(error.what()),
				"settings_check_deferred");
		}
	}
}

void ModbusService::on_stop() noexcept
{
	stopping_ = true;
	if (settings_watcher_.joinable())
		settings_watcher_.join();
	stop_transports();
}

mnc::ServiceHealth ModbusService::health() const
{
	if (failed_)
		return {false, "Modbus I/O worker failed"};
	std::scoped_lock lock(settings_mutex_);
	return {true, active_settings_.enabled
		? "Modbus transports are active" : "Modbus is disabled"};
}

void ModbusService::report_transport_error(std::string_view transport,
	std::string_view message)
{
	(void)logger().write(mnc::logging::Priority::warning,
		"Modbus " + std::string(transport) + ": " + std::string(message),
		"transport_error");
}

} // namespace msap1::modbus::daemon
