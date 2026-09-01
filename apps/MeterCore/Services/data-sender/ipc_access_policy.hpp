#pragma once

#include "mnc/ipc/ipc.hpp"
#include "msap1/datalogger/data_sender_ipc.hpp"

#include <cstdint>
#include <optional>
#include <pwd.h>
#include <string>
#include <string_view>

namespace msap1::datalogger::daemon {

/** Commands that can change outbox state or initiate an outbound connection. */
[[nodiscard]] inline bool administrator_command(ipc::Command command) noexcept
{
	return command == ipc::Command::retry_artifacts ||
		command == ipc::Command::delete_artifacts ||
		command == ipc::Command::test_channel;
}

/** Deterministic core used by the runtime policy and its unit test. */
[[nodiscard]] inline bool command_authorized_for_uids(ipc::Command command,
	std::uint32_t peer_uid, std::optional<std::uint32_t> web_uid,
	std::optional<std::uint32_t> settings_uid) noexcept
{
	if (peer_uid == 0)
		return true;
	if (command == ipc::Command::validate_channels)
		return settings_uid && peer_uid == *settings_uid;
	if (administrator_command(command))
		return web_uid && peer_uid == *web_uid;
	return true;
}

[[nodiscard]] inline std::optional<std::uint32_t> account_uid(
	std::string_view name)
{
	const auto *account = ::getpwnam(std::string(name).c_str());
	if (!account)
		return std::nullopt;
	return static_cast<std::uint32_t>(account->pw_uid);
}

/**
 * Read/list/download is available to peers that can open the group-restricted
 * socket. Mutations and zero-data probes are accepted only from the trusted
 * Web adapter; queued-channel validation is accepted only from settings.
 */
[[nodiscard]] inline bool command_authorized(ipc::Command command,
	const mnc::ipc::PeerCredentials &peer)
{
	return command_authorized_for_uids(command, peer.uid,
		account_uid("mnc-web"), account_uid("mnc-settings"));
}

} // namespace msap1::datalogger::daemon
