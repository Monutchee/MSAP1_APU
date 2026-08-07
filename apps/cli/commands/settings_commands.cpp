#include "core/cli.hpp"
#include "core/result_output.hpp"

#include "msap1/settings/settings_ipc.hpp"

#include <ostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace msap1::cli {
namespace {

using IpcCommand = msap1::settings::ipc::Command;
using msap1::settings::ipc::Request;
using msap1::settings::ipc::Response;
using msap1::settings::ipc::SettingsClient;
using msap1::settings::ipc::Status;

Response request(const Options &options, Request value)
{
	SettingsClient client;
	auto response = client.request(std::move(value), options.timeout_ms);
	if (response.status != Status::ok)
		throw std::runtime_error(response.message.empty()
			? "settings service rejected the request" : response.message);
	return response;
}

int show(const Options &options, std::ostream &output)
{
	Request value;
	value.command = IpcCommand::get_active;
	const auto response = request(options, std::move(value));
	if (options.output_format == OutputFormat::json) {
		output << response.json;
		if (response.json.empty() || response.json.back() != '\n')
			output << '\n';
		return 0;
	}
	output << "MSAP1 active settings\n  Content hash:         "
	       << response.content_hash << "\n\n" << response.json;
	if (response.json.empty() || response.json.back() != '\n')
		output << '\n';
	return 0;
}

int factory_reset(const Options &options, std::ostream &output)
{
	if (!options.settings_confirm)
		throw std::invalid_argument("factory reset requires --confirm");
	Request value;
	value.command = IpcCommand::factory_reset;
	value.confirmed = true;
	const auto response = request(options, std::move(value));
	if (options.output_format == OutputFormat::json)
		write_json_success(output, response);
	else
		output << "Factory settings restored\n  Content hash:         "
		       << response.content_hash << '\n';
	return 0;
}

OptionSpec flag(std::string name, std::string summary,
		std::function<void(Options &)> apply)
{
	return {std::move(name), {}, std::move(summary), CompletionKind::none,
		[apply = std::move(apply)](Options &options, const std::string &) {
			apply(options);
		}, false};
}

CommandMetadata metadata(AccessLevel access, SideEffect effect)
{
	return {.access = access, .side_effect = effect, .supports_text = true,
		.supports_json = true, .variants = {}};
}

} // namespace

void register_settings_commands(Application &application)
{
	Command settings("settings", "Inspect and manage persistent product settings");
	settings.add_subcommand(Command("show", "Show active settings", show,
		metadata(AccessLevel::diagnostic, SideEffect::none)));
	Command reset_command("factory-reset", "Restore packaged factory settings",
		factory_reset,
		metadata(AccessLevel::maintenance, SideEffect::control));
	reset_command.add_option(flag("confirm", "Confirm destructive reset",
		[](Options &options) { options.settings_confirm = true; }));
	settings.add_subcommand(std::move(reset_command));
	application.add_command(std::move(settings));
}

} // namespace msap1::cli
