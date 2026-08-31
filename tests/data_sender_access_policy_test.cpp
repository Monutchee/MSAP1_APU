#include "ipc_access_policy.hpp"

#include <optional>
#include <stdexcept>

namespace {

void require(bool condition, std::string_view message)
{
	if (!condition)
		throw std::runtime_error(std::string(message));
}

void command_scopes_are_pinned()
{
	using namespace msap1::datalogger;
	constexpr std::uint32_t web = 784;
	constexpr std::uint32_t settings = 781;
	constexpr std::uint32_t unrelated = 782;
	const auto allowed = [=](ipc::Command command, std::uint32_t uid) {
		return daemon::command_authorized_for_uids(command, uid, web, settings);
	};

	require(allowed(ipc::Command::get_status, unrelated) &&
		allowed(ipc::Command::read_artifact_chunk, unrelated),
		"group-authorized read command was rejected");
	require(allowed(ipc::Command::delete_artifacts, web) &&
		allowed(ipc::Command::retry_artifacts, web) &&
		allowed(ipc::Command::test_channel, web),
		"trusted Web adapter lost Data Sender administration access");
	require(!allowed(ipc::Command::delete_artifacts, unrelated) &&
		!allowed(ipc::Command::test_channel, settings),
		"non-Web service gained Data Sender mutation access");
	require(allowed(ipc::Command::validate_channels, settings) &&
		!allowed(ipc::Command::validate_channels, web),
		"queued-channel validation is not settings-authority-only");
	require(daemon::command_authorized_for_uids(
		ipc::Command::delete_artifacts, 0, std::nullopt, std::nullopt),
		"root recovery access was rejected");
}

} // namespace

int main()
{
	command_scopes_are_pinned();
}
