#pragma once

#include "mnc/ipc/ipc.hpp"

#include <cstdint>
#include <optional>
#include <pwd.h>
#include <string>

namespace msap1::waveform::daemon {

[[nodiscard]] inline std::optional<std::uint32_t> account_uid(
	std::string_view name)
{
	const auto *account = ::getpwnam(std::string(name).c_str());
	if (!account)
		return std::nullopt;
	return static_cast<std::uint32_t>(account->pw_uid);
}

[[nodiscard]] inline bool peer_authorized_for_uids(std::uint32_t peer_uid,
	std::optional<std::uint32_t> web_uid) noexcept
{
	return peer_uid == 0u || (web_uid && peer_uid == *web_uid);
}

/** Only root and the authenticated Web adapter may use converter IPC. */
[[nodiscard]] inline bool peer_authorized(
	const mnc::ipc::PeerCredentials &peer)
{
	return peer_authorized_for_uids(peer.uid, account_uid("mnc-web"));
}

} // namespace msap1::waveform::daemon
