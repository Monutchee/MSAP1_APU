#include "mnc/ipc/ipc.hpp"
#include "mnc/service.hpp"
#include "mnc/service_manager.hpp"
#include "msap1/service_control.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <atomic>
#include <array>
#include <chrono>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

#include <sys/stat.h>

namespace {

using namespace std::chrono_literals;

class ServiceManagerDaemon final : public mnc::Service {
public:
	ServiceManagerDaemon()
		: Service("MSAP1 service manager", "service-manager"),
		  manager_(mnc::make_systemd_unit_controller()),
		  server_(context_.get_executor(),
			  std::string(msap1::service_control::socket_path))
	{
		manager_.register_service({"fpga-acquisition",
			"msap1-fpga-acquisition.service", {}});
		manager_.register_service({"web-backend",
			"msap1-web-backend.service", {"fpga-acquisition"}});
	}

protected:
	void on_start() override
	{
		manager_.start_registered();
		server_.start(
			[this](auto connection, auto frame) {
				handle_request(std::move(connection), std::move(frame));
			},
			[this](const std::string &message) {
				(void)logger().write(mnc::logging::Priority::warning,
					"service-manager IPC error: " + message,
					"ipc_error");
			});
		/* Read-only operations are available to diagnostic users. Mutating
		 * requests are still authorized with SO_PEERCRED in handle_request(). */
		if (::chmod(msap1::service_control::socket_path.data(), 0666) != 0)
			throw std::runtime_error("cannot set service-manager socket mode");
		schedule_audit();
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

	void on_reload() override
	{
		manager_.start_registered();
		audit_services();
	}

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
			return {false, "service-manager worker failed"};
		return {true, degraded_ ? "managed service is degraded"
					 : "managed services are healthy"};
	}

private:
	static bool is_control(msap1::service_control::Command command)
	{
		using Command = msap1::service_control::Command;
		return command == Command::start || command == Command::stop ||
		       command == Command::restart || command == Command::reload;
	}

	static mnc::ServiceAction action(msap1::service_control::Command command)
	{
		using Command = msap1::service_control::Command;
		switch (command) {
		case Command::start: return mnc::ServiceAction::start;
		case Command::stop: return mnc::ServiceAction::stop;
		case Command::restart: return mnc::ServiceAction::restart;
		case Command::reload: return mnc::ServiceAction::reload;
		default: throw std::invalid_argument("command is not a control action");
		}
	}

	void handle_request(mnc::ipc::UnixStreamServer::Connection connection,
			    mnc::ipc::Frame frame)
	{
		using namespace msap1::service_control;
		Response response;
		Command command = Command::list;
		try {
			const auto request = decode_request(frame);
			command = request.command;
			if (is_control(command) &&
			    connection->peer_credentials().uid != 0) {
				response.status = Status::permission_denied;
				response.message = "service control requires root";
			} else if (command == Command::list) {
				response.services = manager_.statuses();
			} else if (command == Command::status) {
				response.services.push_back(manager_.status(request.service));
			} else {
				response.services.push_back(
					manager_.control(request.service, action(command)));
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

	void schedule_audit()
	{
		audit_timer_.expires_after(5s);
		audit_timer_.async_wait([this](const boost::system::error_code &error) {
			if (error)
				return;
			audit_services();
			schedule_audit();
		});
	}

	void audit_services()
	{
		bool degraded = false;
		for (const auto &status : manager_.statuses()) {
			if (status.active_state == "active" &&
			    !status.permanently_failed)
				continue;
			degraded = true;
			if (status.permanently_failed) {
				const std::array fields{
					mnc::logging::Field{"MNC_SERVICE", status.name},
					mnc::logging::Field{
						"MNC_RESTART_COUNT",
						std::to_string(status.restart_count)}};
				(void)logger().write(mnc::logging::Priority::critical,
					status.name + " exhausted its restart policy",
					"managed_service_failed",
					fields);
			}
		}
		degraded_ = degraded;
	}

	mnc::ServiceManager manager_;
	boost::asio::io_context context_;
	mnc::ipc::UnixStreamServer server_;
	boost::asio::steady_timer audit_timer_{context_.get_executor()};
	std::thread worker_;
	std::exception_ptr failure_;
	std::atomic<bool> failed_{false};
	std::atomic<bool> degraded_{false};
};

} // namespace

int main()
{
	try {
		ServiceManagerDaemon service;
		return service.execute();
	} catch (const std::exception &) {
		return 1;
	}
}
