#include "mnc/service/service_manager.hpp"

#include <exception>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

class FakeController final : public mnc::UnitController {
public:
	mnc::ManagedServiceStatus inspect(
		const mnc::ManagedService &service) override
	{
		auto found = states.find(service.name);
		if (found != states.end())
			return found->second;
		return {service.name, service.unit, "inactive", "dead", 0, false};
	}

	void control(const mnc::ManagedService &service,
		     mnc::ServiceAction action) override
	{
		controls.emplace_back(service.name, action);
		auto &state = states[service.name];
		state.name = service.name;
		state.unit = service.unit;
		if (action == mnc::ServiceAction::stop) {
			state.active_state = "inactive";
			state.sub_state = "dead";
		} else {
			state.active_state = "active";
			state.sub_state = "running";
		}
	}

	std::map<std::string, mnc::ManagedServiceStatus> states;
	std::vector<std::pair<std::string, mnc::ServiceAction>> controls;
};

void dependency_order_and_adoption()
{
	auto controller = std::make_shared<FakeController>();
	controller->states["fpga-acquisition"] = {
		"fpga-acquisition", "acquisition.service", "active", "running", 0,
		false};
	mnc::ServiceManager manager(controller);
	manager.register_service(
		{"web-backend", "web.service", {"fpga-acquisition"}});
	manager.register_service(
		{"fpga-acquisition", "acquisition.service", {}});
	manager.start_registered();
	require(controller->controls.size() == 1 &&
		controller->controls.front().first == "web-backend" &&
		controller->controls.front().second == mnc::ServiceAction::start,
		"manager did not adopt an active dependency before starting consumer");

	const auto statuses = manager.statuses();
	require(statuses.size() == 2 && statuses[0].name == "web-backend" &&
		statuses[1].name == "fpga-acquisition",
		"service registry status order changed");
	const auto stopped =
		manager.control("web-backend", mnc::ServiceAction::stop);
	require(stopped.active_state == "inactive",
		"service control did not return refreshed unit status");

	controller->states["fpga-acquisition"].active_state = "failed";
	controller->states["fpga-acquisition"].sub_state = "failed";
	controller->states["fpga-acquisition"].restart_count = 3;
	controller->states["fpga-acquisition"].permanently_failed = true;
	require(manager.status("fpga-acquisition").permanently_failed,
		"permanently failed unit was not observable");

	bool duplicate_rejected = false;
	try {
		manager.register_service({"duplicate", "web.service", {}});
	} catch (const std::invalid_argument &) {
		duplicate_rejected = true;
	}
	require(duplicate_rejected, "duplicate service unit was accepted");
}

} // namespace

int main()
{
	try {
		dependency_order_and_adoption();
		std::cout << "service manager tests passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "service manager test failed: " << error.what() << '\n';
		return 1;
	}
}
