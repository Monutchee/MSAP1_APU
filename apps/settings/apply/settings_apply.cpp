#include "apply/settings_apply.hpp"

#include "msap1/acquisition/ipc/acquisition_ipc.hpp"

#include <stdexcept>

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

} // namespace msap1::settings::daemon
