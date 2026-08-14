#include "ipc/request_router.hpp"

#include <exception>
#include <pwd.h>
#include <stdexcept>
#include <utility>

namespace msap1::service_manager::daemon {
namespace {

bool control_authorized(const mnc::ipc::PeerCredentials &peer)
{
	if (peer.uid == 0)
		return true;
	const auto *settings_user = ::getpwnam("mnc-settings");
	return settings_user != nullptr && peer.uid == settings_user->pw_uid;
}

} // namespace

RequestRouter::RequestRouter(mnc::ServiceManager &manager) : manager_(manager)
{
}

bool RequestRouter::is_control(msap1::service_control::Command command)
{
	using Command = msap1::service_control::Command;
	return command == Command::start || command == Command::stop ||
	       command == Command::restart || command == Command::reload;
}

mnc::ServiceAction
RequestRouter::action(msap1::service_control::Command command)
{
	using Command = msap1::service_control::Command;
	switch (command) {
	case Command::start: return mnc::ServiceAction::start;
	case Command::stop: return mnc::ServiceAction::stop;
	case Command::restart: return mnc::ServiceAction::restart;
	case Command::reload: return mnc::ServiceAction::reload;
	default:
		throw std::invalid_argument("command is not a control action");
	}
}

void RequestRouter::handle(mnc::ipc::UnixStreamServer::Connection connection,
			   mnc::ipc::Frame frame)
{
	using namespace msap1::service_control;
	Response response;
	Command command = Command::list;
	try {
		const auto request = decode_request(frame);
		command = request.command;
		if (is_control(command) &&
		    !control_authorized(connection->peer_credentials())) {
			response.status = Status::permission_denied;
			response.message = "service control requires root";
		} else if (command == Command::list) {
			response.services = manager_.statuses();
		} else if (command == Command::status) {
			response.services.push_back(
				manager_.status(request.service));
		} else {
			response.services.push_back(manager_.control(
				request.service, action(command)));
		}
	} catch (const std::invalid_argument &error) {
		response.status = Status::invalid_request;
		response.message = error.what();
	} catch (const std::exception &error) {
		response.status = Status::operation_failed;
		response.message = error.what();
	}
	connection->post_send(
		encode_response(response, frame.correlation_id, command));
}

} // namespace msap1::service_manager::daemon
