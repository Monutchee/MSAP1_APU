#include "apply/settings_apply.hpp"

#include "msap1/acquisition/ipc/acquisition_ipc.hpp"
#include "msap1/datalogger/data_sender_ipc.hpp"
#include "msap1/meter/history/historian_ipc.hpp"
#include "msap1/meter/MeterDataProvider/stream/meter_stream_ipc.hpp"
#include "msap1/service/service_control.hpp"

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace msap1::settings::daemon {

void apply_to_acquisition(const msap1::settings::ProductSettings &settings)
{
	msap1::ConfigurationApplyRequest request;
	request.configuration_json =
		msap1::settings::SettingsCodec::encode(settings, false);
	msap1::AcquisitionClient client;
	const auto response = client.request(request, 30000);
	if (response.status != msap1::AcquisitionStatus::ok)
		throw std::runtime_error("acquisition rejected settings apply");
}

void apply_to_database_services(
	const msap1::settings::ProductSettings &settings)
{
	msap1::meter_stream::MeterRecordStreamClient stream;
	const auto spool_policy = settings.database.spool_policy();
	if (stream.policy() != spool_policy)
		stream.apply_policy(spool_policy);

	msap1::history::ipc::HistorianClient historian;
	const auto historian_policies = settings.database.historian_policies();
	if (!mnc::meter_stream::same_database_policies(
		    historian.policies(), historian_policies))
		historian.apply_policies(historian_policies);
}

void apply_to_mqtt_service(const msap1::settings::ProductSettings &settings)
{
	msap1::service_control::Client manager;
	const auto current = manager.request(
		msap1::service_control::Command::status, "mqtt-publisher");
	if (current.status != msap1::service_control::Status::ok ||
	    current.services.size() != 1)
		throw std::runtime_error("cannot inspect MQTT publisher service");
	const auto &active_state = current.services.front().active_state;
	const auto active = active_state == "active" || active_state == "activating";
	msap1::service_control::Command command;
	if (settings.mqtt.enabled)
		command = active ? msap1::service_control::Command::reload
				 : msap1::service_control::Command::start;
	else if (active)
		command = msap1::service_control::Command::stop;
	else
		return;
	const auto response = manager.request(command, "mqtt-publisher", 10000);
	if (response.status != msap1::service_control::Status::ok)
		throw std::runtime_error("MQTT publisher service action failed: " +
			response.message);
}

void apply_to_time_sync_services(
	const msap1::settings::ProductSettings &settings)
{
	msap1::service_control::Client manager;
	const auto set_running = [&manager](std::string_view service,
		bool desired) {
		const auto current = manager.request(
			msap1::service_control::Command::status,
			std::string(service));
		if (current.status != msap1::service_control::Status::ok ||
		    current.services.size() != 1)
			throw std::runtime_error(
				"cannot inspect time synchronization service " +
				std::string(service));
		const auto &state = current.services.front().active_state;
		const auto active = state == "active" || state == "activating";
		if (active == desired)
			return;
		const auto response = manager.request(desired
			? msap1::service_control::Command::start
			: msap1::service_control::Command::stop,
			std::string(service), 10000);
		if (response.status != msap1::service_control::Status::ok)
			throw std::runtime_error(
				"time synchronization service action failed for " +
				std::string(service) + ": " + response.message);
	};

	if (settings.time.synchronization == "ptp") {
		set_running("time-sync-ntp", false);
		set_running("time-sync-ptp-clock", true);
		set_running("time-sync-ptp-system", true);
	} else {
		set_running("time-sync-ptp-system", false);
		set_running("time-sync-ptp-clock", false);
		set_running("time-sync-ntp", true);
	}
}

void apply_to_data_sender_service(
	const msap1::settings::ProductSettings &settings)
{
	msap1::datalogger::ipc::Request validation;
	validation.command = msap1::datalogger::ipc::Command::validate_channels;
	validation.ids.reserve(settings.data_logging.channels.size());
	for (const auto &channel : settings.data_logging.channels)
		validation.ids.push_back(channel.id);
	const msap1::datalogger::ipc::DataSenderClient data_sender;
	const auto validation_response = data_sender.request(
		std::move(validation), 10000);
	if (validation_response.status !=
	    msap1::datalogger::ipc::Status::ok)
		throw std::runtime_error(
			"Data Sender rejected settings apply: " +
			validation_response.message);

	msap1::service_control::Client manager;
	const auto response = manager.request(
		msap1::service_control::Command::reload, "data-sender", 10000);
	if (response.status != msap1::service_control::Status::ok)
		throw std::runtime_error("Data Sender reload failed: " +
			response.message);
}

void apply_to_runtime(const msap1::settings::ProductSettings &settings)
{
	/* Storage is switched before acquisition is reconfigured. If any later
	 * applier fails, SettingsApplyCoordinator invokes this same function with
	 * the previous snapshot and restores both routing and acquisition. */
	apply_to_database_services(settings);
	apply_to_acquisition(settings);
}

} // namespace msap1::settings::daemon
