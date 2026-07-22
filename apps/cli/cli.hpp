#pragma once

#include <cstdint>
#include <functional>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace msap1::cli {

struct Options {
	std::string socket_path;
	std::optional<std::uint64_t> result_limit;
	std::optional<double> duration_seconds;
	int timeout_ms = 3000;
};

enum class CompletionKind { none, path };

struct OptionSpec {
	std::string name;
	std::string value_name;
	std::string summary;
	CompletionKind completion = CompletionKind::none;
	std::function<void(Options &, const std::string &)> apply;
};

class Command {
public:
	using Handler = std::function<int(const Options &, std::ostream &)>;

	Command(std::string name, std::string summary, Handler handler = {});

	Command &add_subcommand(Command command);
	Command &add_option(OptionSpec option);
	const Command *find_subcommand(const std::string &name) const;

	const std::string &name() const { return name_; }
	const std::string &summary() const { return summary_; }
	const std::vector<Command> &subcommands() const { return subcommands_; }
	const std::vector<OptionSpec> &options() const { return options_; }
	const Handler &handler() const { return handler_; }

private:
	std::string name_;
	std::string summary_;
	Handler handler_;
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
		    std::ostream &error) const;
	std::vector<std::string>
	complete(const std::vector<std::string> &arguments) const;
	void write_help(const Command &command, std::ostream &output) const;

	const Command &root() const { return root_; }

private:
	class UsageError;

	const OptionSpec *find_option(const Command &command,
				      const std::string &name) const;
	std::string command_path(const Command &command) const;

	Command root_;
	std::vector<OptionSpec> global_options_;
};

Application make_application();
void register_meter_commands(Application &application);
void register_adc_commands(Application &application);
void request_stop() noexcept;

} // namespace msap1::cli
