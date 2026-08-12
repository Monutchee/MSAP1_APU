/**
 * @file main.cpp
 * @brief Entry point of msap1-settings, the persistent settings authority.
 *
 * The daemon is decomposed by responsibility:
 *  - settings_daemon.*      — service shell: socket server, worker, health
 *  - ipc/request_router.*   — decode -> authorize -> dispatch -> respond
 *  - ipc/access_policy.hpp  — the complete reviewable access-control policy
 *  - apply/settings_apply.* — hot-apply bridge to the acquisition daemon
 */

#include "settings_daemon.hpp"

#include "mnc/logging/logging.hpp"

#include <exception>
#include <string>

int main()
{
	const mnc::logging::Logger lifecycle_log{"settings", "lifecycle"};
	try {
		msap1::settings::daemon::SettingsDaemon service;
		return service.execute();
	} catch (const std::exception &error) {
		/* Startup failures here are almost always the persistent store
		 * being unreadable, so the reason has to reach the journal:
		 * a bare exit(1) reports only "status=1/FAILURE" and hides
		 * whether this was a permission, parse, or socket problem. */
		(void)lifecycle_log.write(mnc::logging::Priority::critical,
			"msap1-settings failed to start: " +
				std::string(error.what()),
			"service_failed");
		return 1;
	} catch (...) {
		(void)lifecycle_log.write(mnc::logging::Priority::critical,
			"msap1-settings failed to start: unknown exception",
			"service_failed");
		return 1;
	}
}
