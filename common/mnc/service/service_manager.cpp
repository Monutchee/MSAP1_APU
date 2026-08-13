#include "mnc/service/service_manager.hpp"

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <utility>

#if defined(MNC_SERVICE_HAVE_SYSTEMD)
#include <systemd/sd-bus.h>
#endif

namespace mnc {
namespace {

#if defined(MNC_SERVICE_HAVE_SYSTEMD)
class SystemdUnitController final : public UnitController {
public:
	SystemdUnitController()
	{
		if (const int error = ::sd_bus_open_system(&bus_); error < 0)
			throw std::runtime_error("cannot connect to systemd system bus");
	}

	~SystemdUnitController() override { ::sd_bus_unref(bus_); }

	ManagedServiceStatus inspect(const ManagedService &service) override
	{
		sd_bus_error error = SD_BUS_ERROR_NULL;
		sd_bus_message *reply = nullptr;
		const int call = ::sd_bus_call_method(bus_,
			"org.freedesktop.systemd1", "/org/freedesktop/systemd1",
			"org.freedesktop.systemd1.Manager", "GetUnit", &error,
			&reply, "s", service.unit.c_str());
		if (call < 0) {
			::sd_bus_error_free(&error);
			::sd_bus_message_unref(reply);
			return {service.name, service.unit, "inactive", "not-found", 0,
				false};
		}
		const char *path = nullptr;
		if (::sd_bus_message_read(reply, "o", &path) < 0 || path == nullptr) {
			::sd_bus_message_unref(reply);
			throw std::runtime_error("systemd returned an invalid unit path");
		}
		std::string unit_path(path);
		::sd_bus_message_unref(reply);

		ManagedServiceStatus result{service.name, service.unit, {}, {}};
		char *active = nullptr;
		char *sub = nullptr;
		(void)::sd_bus_get_property_string(bus_, "org.freedesktop.systemd1",
			unit_path.c_str(), "org.freedesktop.systemd1.Unit",
			"ActiveState", &error, &active);
		::sd_bus_error_free(&error);
		error = SD_BUS_ERROR_NULL;
		(void)::sd_bus_get_property_string(bus_, "org.freedesktop.systemd1",
			unit_path.c_str(), "org.freedesktop.systemd1.Unit", "SubState",
			&error, &sub);
		::sd_bus_error_free(&error);
		result.active_state = active ? active : "unknown";
		result.sub_state = sub ? sub : "unknown";
		std::free(active);
		std::free(sub);

		std::uint32_t restarts = 0;
		(void)::sd_bus_get_property_trivial(bus_, "org.freedesktop.systemd1",
			unit_path.c_str(), "org.freedesktop.systemd1.Service", "NRestarts",
			&error, 'u', &restarts);
		::sd_bus_error_free(&error);
		result.restart_count = restarts;
		result.permanently_failed = result.active_state == "failed" &&
			restarts >= 3;
		return result;
	}

	void control(const ManagedService &service, ServiceAction action) override
	{
		const char *method = nullptr;
		switch (action) {
		case ServiceAction::start: method = "StartUnit"; break;
		case ServiceAction::stop: method = "StopUnit"; break;
		case ServiceAction::restart: method = "RestartUnit"; break;
		case ServiceAction::reload: method = "ReloadUnit"; break;
		}
		sd_bus_error error = SD_BUS_ERROR_NULL;
		sd_bus_message *reply = nullptr;
		const int result = ::sd_bus_call_method(bus_,
			"org.freedesktop.systemd1", "/org/freedesktop/systemd1",
			"org.freedesktop.systemd1.Manager", method, &error, &reply, "ss",
			service.unit.c_str(), "replace");
		::sd_bus_message_unref(reply);
		if (result < 0) {
			const std::string message = error.message ? error.message
								: "systemd control failed";
			::sd_bus_error_free(&error);
			throw std::runtime_error(message);
		}
		::sd_bus_error_free(&error);
	}

private:
	sd_bus *bus_ = nullptr;
};
#else
class SystemdUnitController final : public UnitController {
public:
	ManagedServiceStatus inspect(const ManagedService &service) override
	{
		return {service.name, service.unit, "unavailable", "libsystemd-missing",
			0, true};
	}
	void control(const ManagedService &, ServiceAction) override
	{
		throw std::runtime_error("systemd sd-bus support is unavailable");
	}
};
#endif

} // namespace

std::shared_ptr<UnitController> make_systemd_unit_controller()
{
	return std::make_shared<SystemdUnitController>();
}

ServiceManager::ServiceManager(std::shared_ptr<UnitController> controller)
	: controller_(std::move(controller))
{
	if (!controller_)
		throw std::invalid_argument("service manager requires a unit controller");
}

void ServiceManager::register_service(ManagedService service)
{
	std::scoped_lock lock(mutex_);
	if (std::ranges::any_of(services_, [&](const auto &candidate) {
		    return candidate.name == service.name || candidate.unit == service.unit;
	    }))
		throw std::invalid_argument("duplicate managed service");
	services_.push_back(std::move(service));
}

ManagedService ServiceManager::find(std::string_view name) const
{
	const auto found = std::ranges::find_if(services_, [&](const auto &service) {
		return service.name == name;
	});
	if (found == services_.end())
		throw std::invalid_argument("unknown service: " + std::string(name));
	return *found;
}

void ServiceManager::start_one(const ManagedService &service,
			       std::vector<std::string> &started)
{
	if (std::ranges::find(started, service.name) != started.end())
		return;
	for (const auto &dependency : service.after)
		start_one(find(dependency), started);
	const auto state = controller_->inspect(service);
	if (state.active_state != "active")
		controller_->control(service, ServiceAction::start);
	started.push_back(service.name);
}

void ServiceManager::start_registered()
{
	std::scoped_lock lock(mutex_);
	std::vector<std::string> started;
	for (const auto &service : services_)
		if (service.auto_start)
			start_one(service, started);
}

std::vector<ManagedServiceStatus> ServiceManager::statuses()
{
	std::scoped_lock lock(mutex_);
	std::vector<ManagedServiceStatus> result;
	result.reserve(services_.size());
	for (const auto &service : services_) {
		auto status = controller_->inspect(service);
		status.required_active = service.auto_start;
		result.push_back(std::move(status));
	}
	return result;
}

ManagedServiceStatus ServiceManager::status(std::string_view name)
{
	std::scoped_lock lock(mutex_);
	const auto service = find(name);
	auto status = controller_->inspect(service);
	status.required_active = service.auto_start;
	return status;
}

ManagedServiceStatus ServiceManager::control(std::string_view name,
					     ServiceAction action)
{
	std::scoped_lock lock(mutex_);
	const auto service = find(name);
	controller_->control(service, action);
	auto status = controller_->inspect(service);
	status.required_active = service.auto_start;
	return status;
}

} // namespace mnc
