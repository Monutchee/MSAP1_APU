#pragma once

/**
 * @file access_policy.hpp
 * @brief The complete access-control policy of the settings authority.
 *
 * Everything security-reviewable about the settings IPC lives in this one
 * header: which commands mutate state, which are administrator-only, which
 * remain available in recovery mode, and how peer credentials map to access
 * levels. The functions are pure so the policy can be read (and unit
 * tested) without touching the transport.
 */

#include "mnc/ipc/ipc.hpp"
#include "msap1/settings/settings_ipc.hpp"

#include <cstdint>
#include <pwd.h>
#include <string_view>

#include <unistd.h>

namespace msap1::settings::daemon {

using msap1::settings::ipc::Command;
using msap1::settings::ipc::Status;

/** @brief Commands that require administrator access. */
[[nodiscard]] inline bool administrator_only(Command command)
{
	return command == Command::factory_reset ||
	       command == Command::set_secret ||
	       command == Command::set_named_secret ||
	       command == Command::clear_named_secret ||
	       command == Command::put_asset ||
	       command == Command::delete_asset ||
	       command == Command::download_asset;
}

/** Commands that expose runtime-only credentials to the MQTT service. */
[[nodiscard]] inline bool mqtt_runtime_only(Command command)
{
	return command == Command::resolve_mqtt_credentials ||
	       command == Command::resolve_mqtt_assets;
}

/** @brief Commands that change persistent state. */
[[nodiscard]] inline bool mutation_command(Command command)
{
	return command == Command::save_active ||
	       command == Command::factory_reset ||
	       command == Command::set_secret ||
	       command == Command::set_named_secret ||
	       command == Command::clear_named_secret ||
	       command == Command::put_asset ||
	       command == Command::delete_asset;
}

/**
 * @brief Commands that stay available while the authority is in recovery
 *        mode: read-only diagnostics plus the factory reset that repairs it.
 */
[[nodiscard]] inline bool allowed_during_recovery(Command command)
{
	return command == Command::get_active ||
	       command == Command::get_secret_status ||
	       command == Command::factory_reset;
}

/** Access levels derived from the requesting peer's socket credentials. */
struct PeerAccess {
	/** May issue mutation commands (save, reset, secrets). */
	bool operator_access = false;
	/** May issue administrator-only commands. */
	bool administrator = false;
};

/**
 * @brief Map SO_PEERCRED credentials onto settings access levels.
 *
 * Read-only diagnostics are available to every peer that can open the
 * socket. Mutations require root or a process whose EFFECTIVE group is the
 * settings authority group. This deliberately does not promote
 * supplementary diagnostic-group membership to write access. The trusted
 * Web adapter enforces the authenticated product role before forwarding
 * mutations.
 */
[[nodiscard]] inline PeerAccess
evaluate_peer(const mnc::ipc::PeerCredentials &credentials)
{
	const auto service_group = static_cast<std::uint32_t>(::getegid());
	const bool trusted = credentials.uid == 0u ||
			     credentials.gid == service_group;
	return {trusted, trusted};
}

/**
 * Permit credential resolution only to root or the dedicated MQTT account.
 * Group membership is deliberately insufficient: the Web adapter belongs to
 * the settings group so it can forward authenticated mutations, but it must
 * never receive broker passwords or private-key material.
 */
[[nodiscard]] inline bool
may_resolve_mqtt_runtime(const mnc::ipc::PeerCredentials &credentials)
{
	if (credentials.uid == 0u)
		return true;
	const auto *account = ::getpwnam("mnc-mqtt");
	return account != nullptr &&
	       credentials.uid == static_cast<std::uint32_t>(account->pw_uid);
}

/**
 * @brief Map a handler exception onto the IPC status the client receives.
 *
 * The SettingsHandler reports failures as exceptions with well-known
 * message fragments; this keeps the wire status stable without threading a
 * status code through every internal layer.
 */
[[nodiscard]] inline Status exception_status(std::string_view message)
{
	if (message.find("recovery mode") != std::string_view::npos)
		return Status::recovery_mode;
	if (message.find("stale") != std::string_view::npos)
		return Status::conflict;
	if (message.find("rejected settings apply") != std::string_view::npos ||
	    message.find("settings verification failed") !=
		    std::string_view::npos)
		return Status::apply_failed;
	return Status::internal_error;
}

} // namespace msap1::settings::daemon
