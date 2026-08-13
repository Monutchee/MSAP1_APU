#include "apply/settings_apply.hpp"

#include "msap1/acquisition/ipc/acquisition_ipc.hpp"
#include "msap1/meter/history/historian_ipc.hpp"
#include "msap1/meter/MeterDataProvider/stream/meter_stream_ipc.hpp"

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
	stream.apply_policy(settings.database.spool_policy());
	msap1::history::ipc::HistorianClient{}.apply_policies(
		settings.database.historian_policies());
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
