#pragma once

/**
 * @file settings_daemon.hpp
 * @brief systemd service shell of the MSAP1 settings authority.
 */

#include "mnc/ipc/ipc.hpp"
#include "mnc/service/service.hpp"
#include "msap1/settings/settings.hpp"
#include "request_router.hpp"

#include <boost/asio/io_context.hpp>

#include <atomic>
#include <exception>
#include <thread>

namespace msap1::settings::daemon {

/**
 * @brief Wires the settings authority together and runs its lifecycle.
 *
 * Composition: the SettingsHandler owns the persistent documents and
 * recovery policy (hot-applying every candidate snapshot through
 * apply_to_acquisition() before persisting it); the RequestRouter serves
 * the IPC protocol; this class owns the Unix socket server and the single
 * Asio worker thread, and reports recovery mode and worker failures
 * through the mnc::Service health interface.
 */
class SettingsDaemon final : public mnc::Service {
public:
	SettingsDaemon();

protected:
	/** @brief Initialize documents, start serving, restrict the socket. */
	void on_start() override;
	void on_reload() override {}
	/** @brief Stop the server and join the worker thread. */
	void on_stop() noexcept override;
	/** @brief Healthy while the worker runs; names recovery mode. */
	[[nodiscard]] mnc::ServiceHealth health() const override;

private:
	msap1::settings::SettingsHandler handler_;
	RequestRouter router_;
	boost::asio::io_context context_;
	mnc::ipc::UnixStreamServer server_;
	std::thread worker_;
	std::exception_ptr failure_;
	std::atomic<bool> failed_{false};
};

} // namespace msap1::settings::daemon
