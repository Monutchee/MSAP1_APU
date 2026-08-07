#include "cli.hpp"
#include "result_output.hpp"

#include "msap1/service/service_control.hpp"

#include <iomanip>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace msap1::cli {
namespace {

struct ServiceStatusDto {
	std::string name;
	std::string unit;
	std::string active_state;
	std::string sub_state;
	std::uint32_t restart_count = 0;
	bool permanently_failed = false;
};

struct ServiceResult {
	std::vector<ServiceStatusDto> services;
};

ServiceResult collect(msap1::service_control::Command command,
		      std::string service = {})
{
	msap1::service_control::Client client;
	const auto response = client.request(command, std::move(service));
	if (response.status != msap1::service_control::Status::ok)
		throw std::runtime_error(response.message.empty()
			? "service-manager operation failed" : response.message);
	ServiceResult result;
	for (const auto &status : response.services)
		result.services.push_back({
			.name = status.name,
			.unit = status.unit,
			.active_state = status.active_state,
			.sub_state = status.sub_state,
			.restart_count = status.restart_count,
			.permanently_failed = status.permanently_failed,
		});
	return result;
}

class ServiceTextGenerator final : public ResultGenerator<ServiceResult> {
public:
	int write(const ServiceResult &result, std::ostream &output) const override
	{
		output << std::left << std::setw(20) << "SERVICE"
		       << std::setw(12) << "ACTIVE" << std::setw(16) << "STATE"
		       << std::setw(10) << "RESTARTS" << "FAULT\n";
		for (const auto &service : result.services)
			output << std::left << std::setw(20) << service.name
			       << std::setw(12) << service.active_state
			       << std::setw(16) << service.sub_state
			       << std::setw(10) << service.restart_count
			       << (service.permanently_failed ? "permanent" : "none")
			       << '\n';
		return 0;
	}
};

class ServiceJsonGenerator final : public ResultGenerator<ServiceResult> {
public:
	int write(const ServiceResult &result, std::ostream &output) const override
	{
		write_json_success(output, result);
		return 0;
	}
};

int render(const Options &options, std::ostream &output,
	   msap1::service_control::Command command, std::string service = {})
{
	return render_result(options, collect(command, std::move(service)), output,
		ServiceTextGenerator{}, ServiceJsonGenerator{});
}

Command service_target(std::string name,
		       msap1::service_control::Command command,
		       AccessLevel access)
{
	const auto summary = std::string(command ==
		msap1::service_control::Command::status ? "Inspect " : "Control ") +
		name;
	return Command(name, summary,
		[command, service = name](const Options &options, std::ostream &output) {
			return render(options, output, command, service);
		},
		{
			.access = access,
			.side_effect = access == AccessLevel::diagnostic
				? SideEffect::none : SideEffect::control,
			.supports_text = true,
			.supports_json = true,
			.variants = {},
		});
}

Command target_group(std::string name, std::string summary,
		     msap1::service_control::Command command,
		     AccessLevel access)
{
	Command group(std::move(name), std::move(summary));
	group.add_subcommand(service_target("settings", command, access));
	group.add_subcommand(service_target("fpga-acquisition", command, access));
	group.add_subcommand(service_target("web-backend", command, access));
	return group;
}

} // namespace

void register_service_commands(Application &application)
{
	using ServiceCommand = msap1::service_control::Command;
	Command service("service", "Inspect and control MSAP1 system services");
	service.add_subcommand(Command(
		"list", "List registered services",
		[](const Options &options, std::ostream &output) {
			return render(options, output, ServiceCommand::list);
		},
		{
			.access = AccessLevel::diagnostic,
			.side_effect = SideEffect::none,
			.supports_text = true,
			.supports_json = true,
			.variants = {},
		}));
	service.add_subcommand(target_group("status", "Inspect one service",
		ServiceCommand::status, AccessLevel::diagnostic));
	service.add_subcommand(target_group("start", "Start one service",
		ServiceCommand::start, AccessLevel::maintenance));
	service.add_subcommand(target_group("stop", "Stop one service",
		ServiceCommand::stop, AccessLevel::maintenance));
	service.add_subcommand(target_group("restart", "Restart one service",
		ServiceCommand::restart, AccessLevel::maintenance));
	service.add_subcommand(target_group("reload", "Reload one service",
		ServiceCommand::reload, AccessLevel::maintenance));
	application.add_command(std::move(service));
}

} // namespace msap1::cli
