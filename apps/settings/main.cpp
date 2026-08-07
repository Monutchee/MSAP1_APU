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

int main()
{
	try {
		msap1::settings::daemon::SettingsDaemon service;
		return service.execute();
	} catch (...) {
		return 1;
	}
}
