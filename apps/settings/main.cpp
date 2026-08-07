/**
 * @file main.cpp
 * @brief Entry point of msap1-settings, the persistent settings authority.
 *
 * The daemon is decomposed by responsibility:
 *  - settings_daemon.*  — service shell: socket server, worker thread, health
 *  - request_router.*   — decode -> authorize -> dispatch -> respond + events
 *  - access_policy.hpp  — the complete reviewable access-control policy
 *  - settings_apply.*   — hot-apply bridge to the acquisition daemon
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
