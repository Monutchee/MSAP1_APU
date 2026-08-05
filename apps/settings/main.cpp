#include "mnc/ipc/ipc.hpp"
#include "mnc/service.hpp"
#include "msap1/acquisition_ipc.hpp"
#include "msap1/settings.hpp"
#include "msap1/settings_ipc.hpp"

#include <boost/asio/io_context.hpp>

#include <atomic>
#include <algorithm>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace {

using msap1::settings::ipc::Command;
using msap1::settings::ipc::Response;
using msap1::settings::ipc::Status;

class SettingsDaemon final : public mnc::Service {
public:
	SettingsDaemon()
		: Service("MSAP1 settings", "settings"),
		  handler_(std::filesystem::path(msap1::settings::persistent_root),
			   std::filesystem::path(msap1::settings::factory_defaults_path),
			   msap1::settings::SettingsApplyCoordinator{
				   [this](const auto &settings) { apply(settings); }}),
		  server_(context_.get_executor(),
			  std::string(msap1::settings::socket_path))
	{
	}

protected:
	void on_start() override
	{
		handler_.initialize();
		if (handler_.recovery_mode()) {
			(void)logger().write(mnc::logging::Priority::warning,
				"settings authority entered recovery mode: " +
					handler_.recovery_reason(),
				"settings_recovery_mode");
		} else {
			(void)logger().write(mnc::logging::Priority::notice,
				"settings authority initialized", "settings_ready");
		}
		server_.start(
			[this](auto connection, auto frame) {
				handle_request(std::move(connection), std::move(frame));
			},
			[this](const std::string &message) {
				(void)logger().write(mnc::logging::Priority::warning,
					"settings IPC error: " + message, "ipc_error");
			});
		if (::chmod(msap1::settings::socket_path.data(), 0660) != 0)
			throw std::runtime_error("cannot set settings socket mode");
		worker_ = std::thread([this] {
			try {
				context_.run();
			} catch (...) {
				failure_ = std::current_exception();
				failed_ = true;
				request_stop();
			}
		});
	}

	void on_reload() override {}

	void on_stop() noexcept override
	{
		server_.stop();
		context_.stop();
		if (worker_.joinable())
			worker_.join();
	}

	[[nodiscard]] mnc::ServiceHealth health() const override
	{
		if (failed_)
			return {false, "settings worker failed"};
		if (handler_.recovery_mode())
			return {true, "settings recovery mode: " +
				handler_.recovery_reason()};
		return {true, "settings authority ready"};
	}

private:
	static bool administrator_only(Command command)
	{
		return command == Command::factory_reset ||
		       command == Command::set_secret;
	}

	static bool mutation_command(Command command)
	{
		return command == Command::save_active ||
		       command == Command::factory_reset ||
		       command == Command::set_secret;
	}

	static Response make_event(std::string message, std::string hash = {})
	{
		Response event;
		event.content_hash = std::move(hash);
		event.message = std::move(message);
		return event;
	}

	static bool allowed_during_recovery(Command command)
	{
		return command == Command::get_active ||
		       command == Command::get_secret_status ||
		       command == Command::factory_reset;
	}

	static Status exception_status(std::string_view message)
	{
		if (message.find("recovery mode") != std::string_view::npos)
			return Status::recovery_mode;
		if (message.find("stale") != std::string_view::npos)
			return Status::conflict;
		if (message.find("rejected settings apply") != std::string_view::npos ||
		    message.find("settings verification failed") != std::string_view::npos)
			return Status::apply_failed;
		return Status::internal_error;
	}

	void publish_event(Response event)
	{
		auto frame = msap1::settings::ipc::encode_event(event);
		std::erase_if(subscribers_, [](const auto &subscriber) {
			return subscriber.expired();
		});
		for (const auto &subscriber : subscribers_)
			if (const auto connection = subscriber.lock())
				connection->post_send(frame);
	}

	void apply(const msap1::settings::ProductSettings &settings)
	{
		msap1::AcquisitionRequest request;
		request.command = msap1::AcquisitionCommand::configuration_apply;
		request.sample_rate_hz = settings.metering.sample_rate_hz;
		request.configuration_json =
			msap1::settings::SettingsCodec::encode(settings, false);
		msap1::AcquisitionClient client;
		const auto response = client.request(std::move(request), 30000);
		if (response.status != msap1::AcquisitionStatus::ok)
			throw std::runtime_error("acquisition rejected settings apply");
	}

	void handle_request(mnc::ipc::UnixStreamServer::Connection connection,
			    mnc::ipc::Frame frame)
	{
		Response response;
		std::optional<Response> event;
		Command command = Command::get_active;
		try {
			const auto request = msap1::settings::ipc::decode_request(frame);
			command = request.command;
			const auto credentials = connection->peer_credentials();
			/* Read-only diagnostics are available to every peer that can open
			 * the socket. Mutations require root or a process whose effective
			 * group is the settings authority group.  This deliberately does
			 * not promote supplementary diagnostic-group membership to write
			 * access.  The trusted Web adapter enforces the authenticated
			 * product role before forwarding mutations. */
			const auto service_group = static_cast<std::uint32_t>(::getegid());
			const bool operator_access = credentials.uid == 0u ||
				credentials.gid == service_group;
			const bool administrator = credentials.uid == 0u ||
				credentials.gid == service_group;
			if (handler_.recovery_mode() &&
			    !allowed_during_recovery(command)) {
				response.status = Status::recovery_mode;
				response.message = "settings recovery mode: " +
					handler_.recovery_reason();
			} else if (mutation_command(command) && !operator_access) {
				response.status = Status::permission_denied;
				response.message = "settings mutation requires operator access";
			} else if (administrator_only(command) && !administrator) {
				response.status = Status::permission_denied;
				response.message = "settings mutation requires administrator access";
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

	void dispatch(const msap1::settings::ipc::Request &request,
		      const mnc::ipc::UnixStreamServer::Connection &connection,
		      Response &response, std::optional<Response> &event)
	{
		switch (request.command) {
		case Command::get_active: {
			const auto snapshot = handler_.active();
			response.content_hash = snapshot.content_hash;
			response.json = msap1::settings::SettingsCodec::encode(snapshot.settings);
			if (handler_.recovery_mode()) {
				response.status = Status::recovery_mode;
				response.message = "settings recovery mode: " +
					handler_.recovery_reason();
			}
			break;
		}
		case Command::save_active: {
			const auto settings = msap1::settings::SettingsCodec::decode(request.json);
			const auto snapshot = handler_.save(settings);
			response.content_hash = snapshot.content_hash;
			response.json = msap1::settings::SettingsCodec::encode(snapshot.settings);
			event = make_event("SettingsSaved", snapshot.content_hash);
			break;
		}
		case Command::factory_reset: {
			const auto snapshot = handler_.factory_reset(request.confirmed);
			response.content_hash = snapshot.content_hash;
			response.json = msap1::settings::SettingsCodec::encode(snapshot.settings);
			event = make_event("SettingsFactoryReset", snapshot.content_hash);
			break;
		}
		case Command::set_secret:
			handler_.set_secret_document(request.json);
			event = make_event("SettingsSecretChanged");
			break;
		case Command::get_secret_status:
			response.message = handler_.has_secrets() ? "present" : "absent";
			break;
		case Command::subscribe_events:
			subscribers_.push_back(connection);
			response.message = "subscribed";
			break;
		}
	}

	msap1::settings::SettingsHandler handler_;
	boost::asio::io_context context_;
	mnc::ipc::UnixStreamServer server_;
	std::thread worker_;
	std::exception_ptr failure_;
	std::atomic<bool> failed_{false};
	std::vector<std::weak_ptr<mnc::ipc::FramedConnection>> subscribers_;
};

} // namespace

int main()
{
	try {
		SettingsDaemon service;
		return service.execute();
	} catch (...) {
		return 1;
	}
}
