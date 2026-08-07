#pragma once

/**
 * @file request_router.hpp
 * @brief Decodes, authorizes, dispatches, and answers settings IPC requests.
 */

#include "mnc/ipc/ipc.hpp"
#include "msap1/settings/settings.hpp"
#include "msap1/settings/settings_ipc.hpp"

#include <memory>
#include <optional>
#include <vector>

namespace msap1::settings::daemon {

/**
 * @brief The protocol layer of the settings authority.
 *
 * For every incoming frame the router: decodes the request, applies the
 * access policy (access_policy.hpp) against the peer's socket credentials
 * and the authority's recovery state, dispatches to the SettingsHandler,
 * and sends exactly one correlated response. Mutations additionally
 * publish a change event to every subscribed connection.
 *
 * The router runs entirely on the daemon's single Asio worker thread, so
 * it needs no locking around the subscriber list.
 */
class RequestRouter final {
public:
	/** @param handler The settings authority; must outlive the router. */
	explicit RequestRouter(msap1::settings::SettingsHandler &handler);

	/** @brief Serve one request frame and send its response. */
	void handle(mnc::ipc::UnixStreamServer::Connection connection,
		    mnc::ipc::Frame frame);

private:
	using Response = msap1::settings::ipc::Response;

	/** @brief Execute an authorized command against the handler. */
	void dispatch(const msap1::settings::ipc::Request &request,
		      const mnc::ipc::UnixStreamServer::Connection &connection,
		      Response &response, std::optional<Response> &event);

	/** @brief Send @p event to every live subscriber, pruning dead ones. */
	void publish_event(Response event);

	msap1::settings::SettingsHandler &handler_;
	std::vector<std::weak_ptr<mnc::ipc::FramedConnection>> subscribers_;
};

} // namespace msap1::settings::daemon
