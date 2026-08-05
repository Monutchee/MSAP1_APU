#include "cli.hpp"
#include "result_output.hpp"

#include "msap1/settings.hpp"
#include "msap1/settings_ipc.hpp"

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

void require_ok(const Response &response)
{
	if (response.status != Status::ok)
		throw std::runtime_error(response.message.empty()
			? "settings service rejected the request" : response.message);
}

Response request(const Options &options, Request value)
{
	SettingsClient client;
	auto response = client.request(std::move(value), options.timeout_ms);
	require_ok(response);
	return response;
}

int write_document(const Options &options, const Response &response,
		   std::ostream &output, std::string_view heading)
{
	if (options.output_format == OutputFormat::json) {
		output << response.json;
		if (response.json.empty() || response.json.back() != '\n')
			output << '\n';
		return 0;
	}
	output << heading << "\n  Revision:             " << response.revision;
	if (response.generation != 0u)
		output << "\n  Draft generation:     " << response.generation;
	output << "\n\n" << response.json;
	if (response.json.empty() || response.json.back() != '\n')
		output << '\n';
	return 0;
}

int show(const Options &options, std::ostream &output)
{
	Request value;
	value.command = options.settings_draft ? IpcCommand::get_draft
					      : IpcCommand::get_active;
	return write_document(options, request(options, std::move(value)), output,
		options.settings_draft ? "MSAP1 settings draft"
				       : "MSAP1 active settings");
}

int diff(const Options &options, std::ostream &output)
{
	Request value;
	value.command = IpcCommand::get_diff;
	const auto response = request(options, std::move(value));
	if (options.output_format == OutputFormat::json) {
		write_json_success(output, response.message);
		return 0;
	}
	output << (response.message.empty() ? "No unsaved settings changes.\n"
					   : response.message);
	return 0;
}

int history(const Options &options, std::ostream &output)
{
	Request value;
	value.command = IpcCommand::list_revisions;
	const auto response = request(options, std::move(value));
	if (options.output_format == OutputFormat::json) {
		write_json_success(output, response.message);
		return 0;
	}
	output << "MSAP1 settings revisions\n";
	if (response.message.empty())
		output << "  none\n";
	else
		output << response.message << '\n';
	return 0;
}

int commit(const Options &options, std::ostream &output)
{
	Request draft_request;
	draft_request.command = IpcCommand::get_draft;
	const auto draft = request(options, std::move(draft_request));
	Request value;
	value.command = IpcCommand::commit_draft;
	value.expected_revision = draft.revision;
	value.expected_generation = draft.generation;
	value.message = options.settings_message.value_or("settings update");
	const auto response = request(options, std::move(value));
	if (options.output_format == OutputFormat::json) {
		write_json_success(output, response);
		return 0;
	}
	output << "Settings committed\n  Revision:             "
	       << response.revision << "\n  Transaction:          "
	       << response.transaction_id << '\n';
	return 0;
}

int discard(const Options &options, std::ostream &output)
{
	Request value;
	value.command = IpcCommand::discard_draft;
	(void)request(options, std::move(value));
	if (options.output_format == OutputFormat::json)
		write_json_success(output, std::string{"discarded"});
	else
		output << "Settings draft discarded.\n";
	return 0;
}

int restore(const Options &options, std::ostream &output)
{
	if (!options.settings_revision)
		throw std::invalid_argument("--revision is required");
	Request value;
	value.command = IpcCommand::restore_to_draft;
	value.revision = *options.settings_revision;
	const auto response = request(options, std::move(value));
	return write_document(options, response, output,
		"Revision restored to settings draft");
}

int factory_reset(const Options &options, std::ostream &output)
{
	if (!options.settings_confirm)
		throw std::invalid_argument(
			"factory reset requires --confirm");
	Request value;
	value.command = IpcCommand::factory_reset;
	value.confirmed = true;
	const auto response = request(options, std::move(value));
	if (options.output_format == OutputFormat::json) {
		write_json_success(output, response);
		return 0;
	}
	output << "Factory settings restored\n  Revision:             "
	       << response.revision << "\n  Transaction:          "
	       << response.transaction_id << '\n';
	return 0;
}

int transaction(const Options &options, std::ostream &output)
{
	if (!options.settings_transaction)
		throw std::invalid_argument("--id is required");
	Request value;
	value.command = IpcCommand::get_transaction_status;
	value.message = *options.settings_transaction;
	const auto response = request(options, std::move(value));
	if (options.output_format == OutputFormat::json)
		write_json_success(output, response);
	else
		output << "Settings transaction " << *options.settings_transaction
		       << "\n  " << response.message << '\n';
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
	Command show_command("show", "Show active or draft settings", show,
		metadata(AccessLevel::diagnostic, SideEffect::none));
	show_command.add_option(flag("draft", "Show the persistent draft",
		[](Options &options) { options.settings_draft = true; }));
	settings.add_subcommand(std::move(show_command));
	settings.add_subcommand(Command("diff", "Show unsaved settings changes", diff,
		metadata(AccessLevel::diagnostic, SideEffect::none)));
	settings.add_subcommand(Command("history", "List immutable settings revisions",
		history, metadata(AccessLevel::diagnostic, SideEffect::none)));
	Command commit_command("commit", "Commit and hot-apply the settings draft",
		commit, metadata(AccessLevel::operator_control, SideEffect::control));
	commit_command.add_option({"message", "TEXT", "Revision message",
		CompletionKind::none,
		[](Options &options, const std::string &value) {
			options.settings_message = value;
		}});
	settings.add_subcommand(std::move(commit_command));
	settings.add_subcommand(Command("discard", "Discard the settings draft",
		discard, metadata(AccessLevel::operator_control, SideEffect::control)));
	Command restore_command("restore", "Restore a revision into the draft", restore,
		metadata(AccessLevel::operator_control, SideEffect::control));
	restore_command.add_option({"revision", "NUMBER", "Revision to restore",
		CompletionKind::none,
		[](Options &options, const std::string &value) {
			options.settings_revision = std::stoull(value);
		}});
	settings.add_subcommand(std::move(restore_command));
	Command reset_command("factory-reset", "Restore packaged factory settings",
		factory_reset,
		metadata(AccessLevel::maintenance, SideEffect::control));
	reset_command.add_option(flag("confirm", "Confirm destructive reset",
		[](Options &options) { options.settings_confirm = true; }));
	settings.add_subcommand(std::move(reset_command));
	Command transaction_command("transaction", "Inspect a settings transaction",
		transaction, metadata(AccessLevel::diagnostic, SideEffect::none));
	transaction_command.add_option({"id", "ID", "Transaction identifier",
		CompletionKind::none,
		[](Options &options, const std::string &value) {
			options.settings_transaction = value;
		}});
	settings.add_subcommand(std::move(transaction_command));
	application.add_command(std::move(settings));
}

} // namespace msap1::cli
