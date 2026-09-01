#include "ipc/access_policy.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void require(bool condition, std::string_view message)
{
	if (!condition)
		throw std::runtime_error(std::string(message));
}

void runtime_material_is_service_identity_only()
{
	using namespace msap1::settings;
	constexpr std::uint32_t root = 0;
	constexpr std::uint32_t settings = 781;
	constexpr std::uint32_t web = 784;
	constexpr std::uint32_t mqtt = 786;
	constexpr std::uint32_t data_sender = 787;

	require(daemon::mqtt_runtime_only(
		ipc::Command::resolve_mqtt_credentials) &&
		daemon::data_sender_runtime_only(
			ipc::Command::resolve_data_channel_assets),
		"runtime material command classification changed");
	require(daemon::may_resolve_runtime_for_uids(mqtt, mqtt) &&
		daemon::may_resolve_runtime_for_uids(data_sender, data_sender),
		"dedicated service identity lost credential resolution");
	require(!daemon::may_resolve_runtime_for_uids(web, data_sender) &&
		!daemon::may_resolve_runtime_for_uids(settings, data_sender) &&
		!daemon::may_resolve_runtime_for_uids(
			data_sender, std::nullopt),
		"Web/settings or an unresolved account gained runtime secrets");
	require(daemon::may_resolve_runtime_for_uids(
		root, std::nullopt),
		"root recovery access was rejected");
}

} // namespace

int main()
{
	runtime_material_is_service_identity_only();
}
