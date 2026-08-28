#include "apply/settings_apply.hpp"

#include "msap1/acquisition/ipc/acquisition_ipc.hpp"
#include "msap1/meter/history/historian_ipc.hpp"
#include "msap1/meter/MeterDataProvider/stream/meter_stream_ipc.hpp"
#include "msap1/service/service_control.hpp"

#include <stdexcept>
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

void apply_to_runtime(const msap1::settings::ProductSettings &settings)
{
	/* Storage is switched before acquisition is reconfigured. If any later
	 * applier fails, SettingsApplyCoordinator invokes this same function with
	 * the previous snapshot and restores both routing and acquisition. */
	apply_to_database_services(settings);
	apply_to_acquisition(settings);
}

} // namespace msap1::settings::daemon
