#include "gateway/settings_gateway.hpp"

#include <stdexcept>
#include <utility>

namespace msap1::web {

settings::ipc::Response SettingsGateway::require_ok(
	settings::ipc::Request request, int timeout_ms) const
{
	auto response = client_.request(std::move(request), timeout_ms);
	if (response.status != settings::ipc::Status::ok)
		throw std::runtime_error(response.message.empty()
			? "settings service rejected the request" : response.message);
	return response;
}

settings::ipc::Response SettingsGateway::active(int timeout_ms) const
{
	settings::ipc::Request request;
	request.command = settings::ipc::Command::get_active;
	/* Recovery mode intentionally permits read-only diagnostics.  Mutation
	 * methods still go through require_ok and therefore remain blocked until an
	 * explicit factory reset repairs the authority. */
	auto response = client_.request(std::move(request), timeout_ms);
	if (response.status != settings::ipc::Status::ok &&
	    response.status != settings::ipc::Status::recovery_mode)
		throw std::runtime_error(response.message.empty()
			? "settings service rejected the active settings request"
			: response.message);
	return response;
}

settings::ipc::Response SettingsGateway::save(
	const settings::ProductSettings &value, int timeout_ms) const
{
	settings::ipc::Request request;
	request.command = settings::ipc::Command::save_active;
	request.json = settings::SettingsCodec::encode(value, false);
	return require_ok(std::move(request), timeout_ms);
}

settings::ipc::Response SettingsGateway::factory_reset(
	bool confirmed, int timeout_ms) const
{
	settings::ipc::Request request;
	request.command = settings::ipc::Command::factory_reset;
	request.confirmed = confirmed;
	return require_ok(std::move(request), timeout_ms);
}

settings::ProductSettings SettingsGateway::update_and_save(
	const Mutator &mutator, int timeout_ms) const
{
	const auto current = active(timeout_ms);
	auto value = settings::SettingsCodec::decode(current.json);
	mutator(value);
	(void)save(value, timeout_ms);
	return value;
}

} // namespace msap1::web
