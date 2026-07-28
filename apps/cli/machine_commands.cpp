#include "cli.hpp"
#include "result_output.hpp"

#include <iomanip>
#include <ostream>
#include <string>
#include <vector>

namespace msap1::cli {
namespace {

struct VariantDto {
	std::string selector;
	std::string access;
	bool remote_allowed = false;
	std::string summary;
};

struct CommandDto {
	std::string command;
	std::string summary;
	std::string access;
	std::string side_effect;
	bool remote_allowed = false;
	bool text = true;
	bool json = false;
	std::vector<VariantDto> variants;
};

struct MachineDescription {
	std::uint32_t interface_version = 1;
	std::vector<CommandDto> commands;
};

MachineDescription collect_description()
{
	MachineDescription result;
	for (const auto &descriptor : make_application().descriptors()) {
		CommandDto command{
			.command = descriptor.command,
			.summary = descriptor.summary,
			.access = std::string(
				access_level_name(descriptor.metadata.access)),
			.side_effect = std::string(
				side_effect_name(descriptor.metadata.side_effect)),
			.remote_allowed =
				descriptor.metadata.access == AccessLevel::diagnostic,
			.text = descriptor.metadata.supports_text,
			.json = descriptor.metadata.supports_json,
			.variants = {},
		};
		for (const auto &variant : descriptor.metadata.variants) {
			command.variants.push_back({
				.selector = variant.selector,
				.access =
					std::string(access_level_name(variant.access)),
				.remote_allowed =
					variant.access == AccessLevel::diagnostic,
				.summary = variant.summary,
			});
		}
		result.commands.push_back(std::move(command));
	}
	return result;
}

class MachineTextGenerator final :
	public ResultGenerator<MachineDescription> {
public:
	int write(const MachineDescription &description,
		  std::ostream &output) const override
	{
		output << "MNC machine interface version "
		       << description.interface_version << "\n\n"
		       << std::left << std::setw(28) << "COMMAND"
		       << std::setw(19) << "ACCESS" << std::setw(12) << "REMOTE"
		       << std::setw(9) << "JSON" << "SIDE EFFECT\n";
		for (const auto &command : description.commands) {
			output << std::left << std::setw(28) << command.command
			       << std::setw(19) << command.access
			       << std::setw(12)
			       << (command.remote_allowed ? "yes" : "no")
			       << std::setw(9) << (command.json ? "yes" : "no")
			       << command.side_effect << '\n';
			for (const auto &variant : command.variants)
				output << "  " << std::left << std::setw(26)
				       << variant.selector << std::setw(19)
				       << variant.access << std::setw(12)
				       << (variant.remote_allowed ? "yes" : "no")
				       << std::setw(9) << "-"
				       << variant.summary
				       << '\n';
		}
		return 0;
	}
};

class MachineJsonGenerator final :
	public ResultGenerator<MachineDescription> {
public:
	int write(const MachineDescription &description,
		  std::ostream &output) const override
	{
		write_json_success(output, description);
		return 0;
	}
};

int describe_machine(const Options &options, std::ostream &output)
{
	const auto result = collect_description();
	return render_result(options, result, output, MachineTextGenerator{},
			     MachineJsonGenerator{});
}

} // namespace

void register_machine_commands(Application &application)
{
	Command machine("machine", "Inspect the machine-readable MNC interface");
	machine.add_subcommand(Command(
		"describe", "List commands, access levels, and output capabilities",
		describe_machine,
		{
			.access = AccessLevel::diagnostic,
			.side_effect = SideEffect::none,
			.supports_text = true,
			.supports_json = true,
			.variants = {},
		}));
	application.add_command(std::move(machine));
}

} // namespace msap1::cli
