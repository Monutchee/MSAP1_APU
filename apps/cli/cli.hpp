#pragma once

#include <cstdint>
#include <functional>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace msap1::cli {

enum class OutputFormat { text, json };
enum class AccessLevel {
	diagnostic,
	operator_control,
	maintenance,
	local_only,
};

enum class SideEffect {
	none,
	control,
	destructive_diagnostic,
	continuous,
};

struct Options {
	std::string socket_path;
	OutputFormat output_format = OutputFormat::text;
	std::optional<std::uint64_t> result_limit;
	std::optional<double> duration_seconds;
	std::optional<std::uint32_t> sample_rate_hz;
	std::optional<std::uint32_t> diagnostic_flow;
	std::optional<std::string> log_component;
	std::optional<std::string> log_module;
	std::optional<std::string> log_priority;
	std::optional<std::string> log_since;
	std::optional<std::string> log_cursor;
	bool log_follow = false;
	bool log_json = false;
	bool health_refresh = false;
	bool health_full = false;
	bool socket_overridden = false;
	bool timeout_overridden = false;
	int timeout_ms = 3000;
};

enum class CompletionKind { none, path };

struct OptionSpec {
	std::string name;
	std::string value_name;
	std::string summary;
	CompletionKind completion = CompletionKind::none;
	std::function<void(Options &, const std::string &)> apply;
	bool takes_value = true;
};

struct CommandVariant {
	std::string selector;
	AccessLevel access = AccessLevel::local_only;
	std::string summary;
};

struct CommandMetadata {
	AccessLevel access = AccessLevel::local_only;
	SideEffect side_effect = SideEffect::none;
	bool supports_text = true;
	bool supports_json = false;
	std::vector<CommandVariant> variants;
};

struct CommandDescriptor {
	std::string command;
	std::string summary;
	CommandMetadata metadata;
};

struct ExecutionPolicy {
	AccessLevel maximum_access = AccessLevel::local_only;
	bool require_json = false;
	bool allow_socket_override = true;
	bool allow_timeout_override = true;
};

class Command {
public:
	using Handler = std::function<int(const Options &, std::ostream &)>;
	using AccessResolver = std::function<AccessLevel(const Options &)>;

	Command(std::string name, std::string summary, Handler handler = {},
		CommandMetadata metadata = {});

	Command &add_subcommand(Command command);
	Command &add_option(OptionSpec option);
	Command &set_access_resolver(AccessResolver resolver);
	const Command *find_subcommand(const std::string &name) const;

	const std::string &name() const { return name_; }
	const std::string &summary() const { return summary_; }
	const std::vector<Command> &subcommands() const { return subcommands_; }
	const std::vector<OptionSpec> &options() const { return options_; }
	const Handler &handler() const { return handler_; }
	const CommandMetadata &metadata() const { return metadata_; }
	AccessLevel required_access(const Options &options) const;

private:
	std::string name_;
	std::string summary_;
	Handler handler_;
	CommandMetadata metadata_;
	AccessResolver access_resolver_;
	std::vector<Command> subcommands_;
	std::vector<OptionSpec> options_;
};

struct Invocation {
	const Command *command;
	Options options;
	bool show_help;
};

class Application {
public:
	Application();

	Command &add_command(Command command);
	Invocation parse(const std::vector<std::string> &arguments) const;
	int execute(const std::vector<std::string> &arguments, std::ostream &output,
		    std::ostream &error,
		    const ExecutionPolicy &policy = {}) const;
	std::vector<std::string>
	complete(const std::vector<std::string> &arguments) const;
	void write_help(const Command &command, std::ostream &output) const;
	std::vector<CommandDescriptor> descriptors() const;

	const Command &root() const { return root_; }

private:
	class UsageError;

	const OptionSpec *find_option(const Command &command,
				      const std::string &name) const;
	std::string command_path(const Command &command) const;

	Command root_;
	std::vector<OptionSpec> global_options_;
};

std::string_view access_level_name(AccessLevel level) noexcept;
std::string_view side_effect_name(SideEffect effect) noexcept;
bool access_allowed(AccessLevel required, AccessLevel maximum) noexcept;
bool arguments_request_json(const std::vector<std::string> &arguments) noexcept;
void write_json_error(std::ostream &output, std::string_view code,
		      std::string_view message);

Application make_application();
void register_meter_commands(Application &application);
void register_adc_commands(Application &application);
void register_log_command(Application &application);
void register_system_commands(Application &application);
void register_machine_commands(Application &application);
void request_stop() noexcept;
bool stop_was_requested() noexcept;

} // namespace msap1::cli
