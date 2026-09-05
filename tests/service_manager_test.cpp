#include "mnc/service/service_manager.hpp"
#include "msap1/service/service_control.hpp"
#include "product_units.hpp"

#include <algorithm>
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
		mnc::ManagedServiceStatus result{
			service.name, service.unit, "inactive", "dead", 0, false};
		result.effective_nice =
			mnc::service_priority_nice(service.priority_tier);
		result.priority_matches = true;
		return result;
	}

	void control(const mnc::ManagedService &service,
		     mnc::ServiceAction action) override
	{
		controls.emplace_back(service.name, action);
		auto &state = states[service.name];
		state.name = service.name;
		state.unit = service.unit;
		state.priority_tier = service.priority_tier;
		state.expected_nice =
			mnc::service_priority_nice(service.priority_tier);
		state.effective_nice = state.expected_nice;
		state.priority_matches = true;
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
	controller->states["fpga-acquisition"].priority_matches = true;
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
	require(statuses[1].priority_tier == mnc::ServicePriorityTier::normal &&
		statuses[1].expected_nice == 0 && statuses[1].effective_nice == 0 &&
		statuses[1].priority_matches,
		"service priority status was not decorated");
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

void product_topology_orders_the_data_sender_without_owning_mqtt()
{
	auto controller = std::make_shared<FakeController>();
	mnc::ServiceManager manager(controller);
	msap1::service_manager::daemon::register_product_units(manager);
	manager.start_registered();
	const auto position = [&](std::string_view name) {
		const auto found = std::ranges::find(controller->controls, name,
			[](const auto &item) { return std::string_view(item.first); });
		return found == controller->controls.end()
			? controller->controls.size()
			: static_cast<std::size_t>(found - controller->controls.begin());
	};
	const auto settings = position("settings");
	const auto stream = position("meter-stream");
	const auto historian = position("meter-historian");
	const auto sender = position("data-sender");
	const auto acquisition = position("fpga-acquisition");
	const auto web = position("web-backend");
	require(settings < stream && stream < historian && settings < sender &&
		historian < sender && sender < web && acquisition < web,
		"product topology did not order Data Sender after its durable sources");
	require(position("mqtt-publisher") == controller->controls.size(),
		"settings-controlled MQTT was started unconditionally");
	require(position("time-sync-ntp") == controller->controls.size() &&
		position("time-sync-ptp-clock") == controller->controls.size() &&
		position("time-sync-ptp-system") == controller->controls.size(),
		"settings-controlled time synchronization was started unconditionally");
	const auto acquisition_status = manager.status("fpga-acquisition");
	require(acquisition_status.priority_tier ==
			mnc::ServicePriorityTier::critical &&
		acquisition_status.expected_nice == -10 &&
		acquisition_status.effective_nice == -10 &&
		acquisition_status.priority_matches,
		"FPGA acquisition priority tier is incorrect");
	const auto historian_status = manager.status("meter-historian");
	require(historian_status.priority_tier ==
			mnc::ServicePriorityTier::background &&
		historian_status.expected_nice == 5,
		"historian priority tier is incorrect");
}

void service_protocol_round_trips_priority_status()
{
	msap1::service_control::Response response;
	mnc::ManagedServiceStatus status{
		"fpga-acquisition", "msap1-fpga-acquisition.service", "active",
		"running", 2, false};
	status.required_active = true;
	status.priority_tier = mnc::ServicePriorityTier::critical;
	status.expected_nice = -10;
	status.effective_nice = -10;
	status.priority_matches = true;
	response.services.push_back(status);

	const auto frame = msap1::service_control::encode_response(
		response, 42, msap1::service_control::Command::list);
	const auto decoded = msap1::service_control::decode_response(frame);
	require(decoded.services.size() == 1,
		"service protocol lost a status entry");
	const auto &actual = decoded.services.front();
	require(actual.priority_tier == mnc::ServicePriorityTier::critical &&
		actual.expected_nice == -10 && actual.effective_nice == -10 &&
		actual.priority_matches && actual.required_active,
		"service protocol lost priority status");
}

} // namespace

int main()
{
	try {
		dependency_order_and_adoption();
		product_topology_orders_the_data_sender_without_owning_mqtt();
		service_protocol_round_trips_priority_status();
		std::cout << "service manager tests passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "service manager test failed: " << error.what() << '\n';
		return 1;
	}
}
