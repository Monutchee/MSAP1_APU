#include "cli.hpp"

#include "msap1/acquisition_ipc.hpp"

#include <glaze/glaze.hpp>

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace msap1::cli {
namespace {

constexpr const char *file_completion_marker = "__MNC_FILE__";

struct JsonError {
	std::string code;
	std::string message;
};

struct JsonErrorEnvelope {
	std::string schema = "mnc.response.v1";
	bool success = false;
	JsonError error;
};

std::uint64_t parse_positive_integer(const std::string &value,
				     const std::string &option)
{
	std::size_t end = 0;
	std::uint64_t result = 0;
	try {
		result = std::stoull(value, &end, 0);
	} catch (const std::exception &) {
		throw std::invalid_argument(option + " requires a positive integer");
	}
	if (end != value.size() || result == 0)
		throw std::invalid_argument(option + " requires a positive integer");
	return result;
}

bool find_path(const Command &current, const Command *target,
	       std::vector<std::string> &path)
{
	if (&current == target)
		return true;
	for (const auto &child : current.subcommands()) {
		path.push_back(child.name());
		if (find_path(child, target, path))
			return true;
		path.pop_back();
	}
	return false;
}

void write_option(std::ostream &output, const OptionSpec &option)
{
	const std::string label = "--" + option.name +
		(option.takes_value ? " " + option.value_name : "");
	output << "  " << std::left << std::setw(24) << label << option.summary
	       << '\n';
}

int access_rank(AccessLevel level) noexcept
{
	switch (level) {
	case AccessLevel::diagnostic: return 0;
	case AccessLevel::operator_control: return 1;
	case AccessLevel::maintenance: return 2;
	case AccessLevel::local_only: return 3;
	}
	return 3;
}

void collect_descriptors(const Command &command, const std::string &parent,
			 std::vector<CommandDescriptor> &result)
{
	const auto path = parent.empty() ? command.name()
					: parent + " " + command.name();
	if (command.handler())
		result.push_back({path, command.summary(), command.metadata()});
	for (const auto &child : command.subcommands())
		collect_descriptors(child, path, result);
}

} // namespace

class Application::UsageError : public std::invalid_argument {
public:
	UsageError(std::string message, const Command &command)
		: std::invalid_argument(std::move(message)), command_(&command)
	{
	}

	const Command &command() const { return *command_; }

private:
	const Command *command_;
};

Command::Command(std::string name, std::string summary, Handler handler,
		 CommandMetadata metadata)
	: name_(std::move(name)), summary_(std::move(summary)),
	  handler_(std::move(handler)), metadata_(std::move(metadata))
{
}

Command &Command::add_subcommand(Command command)
{
	subcommands_.push_back(std::move(command));
	return subcommands_.back();
}

Command &Command::add_option(OptionSpec option)
{
	options_.push_back(std::move(option));
	return *this;
}

Command &Command::set_access_resolver(AccessResolver resolver)
{
	access_resolver_ = std::move(resolver);
	return *this;
}

AccessLevel Command::required_access(const Options &options) const
{
	return access_resolver_ ? access_resolver_(options) : metadata_.access;
}

const Command *Command::find_subcommand(const std::string &name) const
{
	const auto found = std::find_if(subcommands_.begin(), subcommands_.end(),
		[&name](const Command &command) { return command.name() == name; });
	return found == subcommands_.end() ? nullptr : &*found;
}

Application::Application()
	: root_("mnc", "Control and inspect the Monutchee MSAP1 system")
{
	global_options_.push_back({
		"socket", "PATH", "Acquisition daemon control socket",
		CompletionKind::path,
		[](Options &options, const std::string &value) {
			options.socket_path = value;
			options.socket_overridden = true;
		},
	});
	global_options_.push_back({
		"timeout-ms", "MS", "Daemon timeout (default: 3000)",
		CompletionKind::none,
		[](Options &options, const std::string &value) {
			const auto timeout = parse_positive_integer(value, "--timeout-ms");
			if (timeout >
			    static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
				throw std::invalid_argument("--timeout-ms is too large");
			options.timeout_ms = static_cast<int>(timeout);
			options.timeout_overridden = true;
		},
	});
	global_options_.push_back({
		"output", "FORMAT", "Output format: text or json",
		CompletionKind::none,
		[](Options &options, const std::string &value) {
			if (value == "text")
				options.output_format = OutputFormat::text;
			else if (value == "json")
				options.output_format = OutputFormat::json;
			else
				throw std::invalid_argument(
					"--output must be text or json");
		},
	});
}

Command &Application::add_command(Command command)
{
	return root_.add_subcommand(std::move(command));
}

const OptionSpec *Application::find_option(const Command &command,
					   const std::string &name) const
{
	const auto local = std::find_if(command.options().begin(),
		command.options().end(), [&name](const OptionSpec &option) {
			return option.name == name;
		});
	if (local != command.options().end())
		return &*local;
	const auto global = std::find_if(global_options_.begin(), global_options_.end(),
		[&name](const OptionSpec &option) { return option.name == name; });
	return global == global_options_.end() ? nullptr : &*global;
}

Invocation Application::parse(const std::vector<std::string> &arguments) const
{
	Options options;
	options.socket_path = msap1::acquisition_socket_path;
	const Command *current = &root_;

	for (std::size_t index = 0; index < arguments.size(); ++index) {
		const auto &argument = arguments[index];
		if (argument == "-h" || argument == "--help")
			return {current, std::move(options), true};
		if (argument == "help") {
			if (current != &root_)
				throw UsageError("unknown command 'help'", *current);
			for (++index; index < arguments.size(); ++index) {
				const auto *child = current->find_subcommand(arguments[index]);
				if (child == nullptr)
					throw UsageError("unknown command '" +
						arguments[index] + "'", *current);
				current = child;
			}
			return {current, std::move(options), true};
		}
		if (!argument.empty() && argument.front() == '-') {
			if (!argument.starts_with("--"))
				throw UsageError("unknown option '" + argument + "'", *current);
			const auto equals = argument.find('=');
			const auto name = argument.substr(2, equals == std::string::npos
				? std::string::npos
				: equals - 2);
			const auto *option = find_option(*current, name);
			if (option == nullptr)
				throw UsageError("unknown option '--" + name + "'", *current);
			std::string value;
			if (!option->takes_value) {
				if (equals != std::string::npos)
					throw UsageError("--" + name +
						" does not take a value", *current);
			} else if (equals != std::string::npos) {
				value = argument.substr(equals + 1);
			} else {
				if (++index >= arguments.size())
					throw UsageError("--" + name + " requires a value",
						*current);
				value = arguments[index];
			}
			try {
				option->apply(options, value);
			} catch (const std::invalid_argument &error) {
				throw UsageError(error.what(), *current);
			}
			continue;
		}

		const auto *child = current->find_subcommand(argument);
		if (child == nullptr) {
			const auto kind = current->subcommands().empty() ? "argument" : "command";
			throw UsageError("unknown " + std::string(kind) + " '" + argument +
				"'", *current);
		}
		current = child;
	}

	return {current, std::move(options), !current->handler()};
}

int Application::execute(const std::vector<std::string> &arguments,
			 std::ostream &output, std::ostream &error,
			 const ExecutionPolicy &policy) const
{
	const bool json_requested = arguments_request_json(arguments) ||
		policy.require_json;
	try {
		auto invocation = parse(arguments);
		if (policy.require_json &&
		    invocation.options.output_format != OutputFormat::json) {
			write_json_error(output, "OUTPUT_REQUIRED",
					 "restricted access requires --output json");
			return 2;
		}
		if (!policy.allow_socket_override &&
		    invocation.options.socket_overridden) {
			write_json_error(
				output, "ACCESS_DENIED",
				"--socket is unavailable through restricted access");
			return 3;
		}
		if (!policy.allow_timeout_override &&
		    invocation.options.timeout_overridden) {
			write_json_error(
				output, "ACCESS_DENIED",
				"--timeout-ms is unavailable through restricted access");
			return 3;
		}
		if (invocation.show_help) {
			if (policy.require_json) {
				write_json_error(
					output, "ACCESS_DENIED",
					"interactive help is unavailable through restricted access");
				return 3;
			}
			write_help(*invocation.command, output);
			return 0;
		}
		const auto required =
			invocation.command->required_access(invocation.options);
		if (!access_allowed(required, policy.maximum_access)) {
			const auto message = "command requires " +
				std::string(access_level_name(required)) +
				" access";
			if (json_requested)
				write_json_error(output, "ACCESS_DENIED", message);
			else
				error << "mnc: " << message << '\n';
			return 3;
		}
		if (invocation.options.output_format == OutputFormat::json &&
		    !invocation.command->metadata().supports_json) {
			const auto message =
				"command does not support machine-readable JSON output";
			write_json_error(output, "OUTPUT_UNSUPPORTED", message);
			return 2;
		}
		return invocation.command->handler()(invocation.options, output);
	} catch (const UsageError &usage_error) {
		if (json_requested) {
			write_json_error(output, "USAGE_ERROR", usage_error.what());
			return 2;
		}
		error << "mnc: " << usage_error.what() << "\n\n";
		write_help(usage_error.command(), error);
		return 2;
	} catch (const std::invalid_argument &invalid) {
		if (json_requested)
			write_json_error(output, "USAGE_ERROR", invalid.what());
		else
			error << "mnc: " << invalid.what() << '\n';
		return 2;
	} catch (const msap1::AcquisitionUnavailable &unavailable) {
		if (json_requested)
			write_json_error(output, "SERVICE_UNAVAILABLE",
					 unavailable.what());
		else
			error << "mnc: " << unavailable.what() << '\n';
		return 4;
	} catch (const std::exception &runtime) {
		if (json_requested)
			write_json_error(output, "RUNTIME_ERROR", runtime.what());
		else
			error << "mnc: " << runtime.what() << '\n';
		return 1;
	}
}

std::string Application::command_path(const Command &command) const
{
	std::vector<std::string> path{root_.name()};
	if (&command != &root_ && !find_path(root_, &command, path))
		throw std::logic_error("command is not registered");
	std::string result;
	for (const auto &part : path) {
		if (!result.empty())
			result += ' ';
		result += part;
	}
	return result;
}

void Application::write_help(const Command &command, std::ostream &output) const
{
	const auto path = command_path(command);
	output << command.summary() << "\n\nUsage:\n  " << path;
	if (&command == &root_)
		output << " [global options] <command>";
	else if (!command.subcommands().empty())
		output << " [global options] <command>";
	else
		output << " [global options] [options]";
	output << "\n";

	if (!command.subcommands().empty() || &command == &root_) {
		output << "\nCommands:\n";
		for (const auto &child : command.subcommands())
			output << "  " << std::left << std::setw(18) << child.name()
			       << child.summary() << '\n';
		if (&command == &root_)
			output << "  " << std::left << std::setw(18) << "help"
			       << "Show help for a command" << '\n';
	}

	if (!command.options().empty()) {
		output << "\nOptions:\n";
		for (const auto &option : command.options())
			write_option(output, option);
	}
	output << "\nGlobal options:\n";
	for (const auto &option : global_options_)
		write_option(output, option);
	output << "  " << std::left << std::setw(24) << "-h, --help"
	       << "Show contextual help\n";
	if (&command == &root_)
		output << "\nUse \"mnc help <command>\" for more information.\n";
}

std::vector<CommandDescriptor> Application::descriptors() const
{
	std::vector<CommandDescriptor> result;
	for (const auto &child : root_.subcommands())
		collect_descriptors(child, root_.name(), result);
	const CommandMetadata local_tool{
		.access = AccessLevel::local_only,
		.side_effect = SideEffect::none,
		.supports_text = true,
		.supports_json = false,
		.variants = {},
	};
	result.push_back({"mnc help", "Show contextual human help", local_tool});
	result.push_back(
		{"mnc __complete", "Generate local shell completions", local_tool});
	std::sort(result.begin(), result.end(),
		  [](const auto &left, const auto &right) {
			  return left.command < right.command;
		  });
	return result;
}

std::vector<std::string>
Application::complete(const std::vector<std::string> &arguments) const
{
	const std::string prefix = arguments.empty() ? "" : arguments.back();
	const std::size_t completed_count =
		arguments.empty() ? 0 : arguments.size() - 1;
	const Command *current = &root_;
	const OptionSpec *pending_value = nullptr;

	for (std::size_t index = 0; index < completed_count; ++index) {
		const auto &argument = arguments[index];
		if (pending_value != nullptr) {
			pending_value = nullptr;
			continue;
		}
		if (argument == "help" && current == &root_)
			continue;
		if (argument.starts_with("--")) {
			const auto equals = argument.find('=');
			const auto name = argument.substr(2, equals == std::string::npos
				? std::string::npos
				: equals - 2);
			const auto *option = find_option(*current, name);
			if (option == nullptr)
				return {};
			if (equals == std::string::npos && option->takes_value)
				pending_value = option;
			continue;
		}
		const auto *child = current->find_subcommand(argument);
		if (child == nullptr)
			return {};
		current = child;
	}

	if (pending_value != nullptr)
		return pending_value->completion == CompletionKind::path
			? std::vector<std::string>{file_completion_marker}
			: std::vector<std::string>{};

	std::vector<std::string> candidates;
	if (prefix.empty() || prefix.front() != '-') {
		for (const auto &child : current->subcommands())
			candidates.push_back(child.name());
		if (current == &root_)
			candidates.emplace_back("help");
	}
	if (prefix.empty() || prefix.front() == '-') {
		for (const auto &option : current->options())
			candidates.push_back("--" + option.name);
		for (const auto &option : global_options_)
			candidates.push_back("--" + option.name);
		candidates.emplace_back("--help");
	}
	std::erase_if(candidates, [&prefix](const std::string &candidate) {
		return !candidate.starts_with(prefix);
	});
	std::sort(candidates.begin(), candidates.end());
	candidates.erase(std::unique(candidates.begin(), candidates.end()),
			 candidates.end());
	return candidates;
}

Application make_application()
{
	Application application;
	register_meter_commands(application);
	register_adc_commands(application);
	register_log_command(application);
	register_system_commands(application);
	register_machine_commands(application);
	return application;
}

std::string_view access_level_name(AccessLevel level) noexcept
{
	switch (level) {
	case AccessLevel::diagnostic: return "diagnostic";
	case AccessLevel::operator_control: return "operator_control";
	case AccessLevel::maintenance: return "maintenance";
	case AccessLevel::local_only: return "local_only";
	}
	return "local_only";
}

std::string_view side_effect_name(SideEffect effect) noexcept
{
	switch (effect) {
	case SideEffect::none: return "none";
	case SideEffect::control: return "control";
	case SideEffect::destructive_diagnostic:
		return "destructive_diagnostic";
	case SideEffect::continuous: return "continuous";
	}
	return "none";
}

bool access_allowed(AccessLevel required, AccessLevel maximum) noexcept
{
	return access_rank(required) <= access_rank(maximum);
}

bool arguments_request_json(
	const std::vector<std::string> &arguments) noexcept
{
	for (std::size_t index = 0; index < arguments.size(); ++index) {
		if (arguments[index] == "--output" && index + 1 < arguments.size() &&
		    arguments[index + 1] == "json")
			return true;
		if (arguments[index] == "--output=json")
			return true;
	}
	return false;
}

void write_json_error(std::ostream &output, std::string_view code,
		      std::string_view message)
{
	const auto json = glz::write_json(JsonErrorEnvelope{
		.error = {std::string(code), std::string(message)}});
	if (json)
		output << *json << '\n';
	else
		output << R"({"schema":"mnc.response.v1","success":false,"error":{"code":"JSON_ERROR","message":"failed to serialize error"}})"
		       << '\n';
}

} // namespace msap1::cli
