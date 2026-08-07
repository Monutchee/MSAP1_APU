#include "request_router.hpp"

#include "access_policy.hpp"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

namespace msap1::settings::daemon {

namespace {

using msap1::settings::ipc::Response;

/** Build a change event carrying the new content hash. */
Response make_event(std::string message, std::string hash = {})
{
	Response event;
	event.content_hash = std::move(hash);
	event.message = std::move(message);
	return event;
}

} // namespace

RequestRouter::RequestRouter(msap1::settings::SettingsHandler &handler)
	: handler_(handler)
{
}

void RequestRouter::handle(mnc::ipc::UnixStreamServer::Connection connection,
			   mnc::ipc::Frame frame)
{
	Response response;
	std::optional<Response> event;
	Command command = Command::get_active;
	try {
		const auto request =
			msap1::settings::ipc::decode_request(frame);
		command = request.command;
		const auto access =
			evaluate_peer(connection->peer_credentials());
		if (handler_.recovery_mode() &&
		    !allowed_during_recovery(command)) {
			response.status = Status::recovery_mode;
			response.message = "settings recovery mode: " +
					   handler_.recovery_reason();
		} else if (mutation_command(command) &&
			   !access.operator_access) {
			response.status = Status::permission_denied;
			response.message =
				"settings mutation requires operator access";
		} else if (administrator_only(command) &&
			   !access.administrator) {
			response.status = Status::permission_denied;
			response.message =
				"settings mutation requires administrator access";
		} else {
			dispatch(request, connection, response, event);
		}
	} catch (const std::invalid_argument &error) {
		response.status = Status::invalid_request;
		response.message = error.what();
	} catch (const std::exception &error) {
		response.status = exception_status(error.what());
		response.message = error.what();
	}
	connection->post_send(msap1::settings::ipc::encode_response(
		response, frame.correlation_id, command));
	if (event)
		publish_event(std::move(*event));
}

void RequestRouter::dispatch(
	const msap1::settings::ipc::Request &request,
	const mnc::ipc::UnixStreamServer::Connection &connection,
	Response &response, std::optional<Response> &event)
{
	switch (request.command) {
	case Command::get_active: {
		const auto snapshot = handler_.active();
		response.content_hash = snapshot.content_hash;
		response.json = msap1::settings::SettingsCodec::encode(
			snapshot.settings);
		/* Recovery mode intentionally still serves the document so
		 * diagnostics can inspect it; the status tells the client. */
		if (handler_.recovery_mode()) {
			response.status = Status::recovery_mode;
			response.message = "settings recovery mode: " +
					   handler_.recovery_reason();
		}
		break;
	}
	case Command::save_active: {
		const auto settings =
			msap1::settings::SettingsCodec::decode(request.json);
		const auto snapshot = handler_.save(settings);
		response.content_hash = snapshot.content_hash;
		response.json = msap1::settings::SettingsCodec::encode(
			snapshot.settings);
		event = make_event("SettingsSaved", snapshot.content_hash);
		break;
	}
	case Command::factory_reset: {
		const auto snapshot = handler_.factory_reset(request.confirmed);
		response.content_hash = snapshot.content_hash;
		response.json = msap1::settings::SettingsCodec::encode(
			snapshot.settings);
		event = make_event("SettingsFactoryReset",
				   snapshot.content_hash);
		break;
	}
	case Command::set_secret:
		handler_.set_secret_document(request.json);
		event = make_event("SettingsSecretChanged");
		break;
	case Command::get_secret_status:
		response.message = handler_.has_secrets() ? "present"
							  : "absent";
		break;
	case Command::subscribe_events:
		subscribers_.push_back(connection);
		response.message = "subscribed";
		break;
	}
}

void RequestRouter::publish_event(Response event)
{
	auto frame = msap1::settings::ipc::encode_event(event);
	std::erase_if(subscribers_, [](const auto &subscriber) {
		return subscriber.expired();
	});
	for (const auto &subscriber : subscribers_)
		if (const auto connection = subscriber.lock())
			connection->post_send(frame);
}

} // namespace msap1::settings::daemon
