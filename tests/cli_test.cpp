#include "cli.hpp"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

bool contains(const std::vector<std::string> &values, const std::string &value)
{
	return std::find(values.begin(), values.end(), value) != values.end();
}

void command_hierarchy()
{
	const auto application = msap1::cli::make_application();
	const auto root = application.parse({});
	require(root.show_help && root.command == &application.root(),
		"empty invocation did not select root help");
	const auto meter = application.parse({"meter"});
	require(meter.show_help && meter.command->name() == "meter",
		"meter group did not select contextual help");
	const auto adc = application.parse({"help", "adc"});
	require(adc.show_help && adc.command->name() == "adc",
		"explicit help did not select the ADC group");
}

void option_parsing()
{
	const auto application = msap1::cli::make_application();
	const auto invocation = application.parse({
		"--timeout-ms", "42", "meter", "view", "--results", "3",
		"--socket", "/tmp/acquisition.sock", "--duration=1.5", "--help",
	});
	require(invocation.show_help && invocation.command->name() == "view",
		"leaf help did not select meter view");
	require(invocation.options.timeout_ms == 42,
		"global option before command was not parsed");
	require(invocation.options.socket_path == "/tmp/acquisition.sock",
		"global option after command was not parsed");
	require(invocation.options.result_limit == 3,
		"result limit was not parsed");
	require(invocation.options.duration_seconds == 1.5,
		"inline duration was not parsed");

	const auto rate =
		application.parse({"adc", "rate", "--sps", "16000"});
	require(!rate.show_help && rate.command->name() == "rate",
		"ADC rate command was not selected");
	require(rate.options.sample_rate_hz == 16000,
		"ADC sample rate was not parsed");
}

void help_and_errors()
{
	const auto application = msap1::cli::make_application();
	std::ostringstream output;
	std::ostringstream error;
	require(application.execute({"meter", "--help"}, output, error) == 0,
		"contextual help failed");
	require(output.str().find("mnc meter") != std::string::npos,
		"contextual help omitted its command path");
	output.str({});
	error.str({});
	require(application.execute({"meter", "unknown"}, output, error) == 2,
		"unknown command did not return a usage error");
	require(error.str().find("unknown command 'unknown'") != std::string::npos,
		"usage error omitted the invalid command");
	output.str({});
	error.str({});
	require(application.execute(
		{"meter", "view", "--duration", "nan"}, output, error) == 2,
		"non-finite duration was accepted");
	output.str({});
	error.str({});
	require(application.execute(
		{"adc", "rate", "--sps", "19200"}, output, error) == 2,
		"unsupported ADC sample rate was accepted");
	require(error.str().find("must be one of") != std::string::npos,
		"sample-rate error omitted the supported values");
}

void completion()
{
	const auto application = msap1::cli::make_application();
	auto candidates = application.complete({""});
	require(contains(candidates, "adc") && contains(candidates, "meter"),
		"root completion omitted command groups");
	candidates = application.complete({"meter", ""});
	require(contains(candidates, "health") && contains(candidates, "view"),
		"meter completion omitted actions");
	candidates = application.complete({"meter", "view", "--"});
	require(contains(candidates, "--duration") &&
		contains(candidates, "--results") && contains(candidates, "--socket"),
		"leaf completion omitted options");
	candidates = application.complete({"meter", "view", "--socket", ""});
	require(candidates == std::vector<std::string>{"__MNC_FILE__"},
		"socket completion did not request path completion");
	candidates = application.complete({"adc", ""});
	require(contains(candidates, "rate") && contains(candidates, "start") &&
		contains(candidates, "stop"),
		"ADC completion omitted actions");
	candidates = application.complete({"adc", "rate", "--"});
	require(contains(candidates, "--sps"),
		"ADC rate completion omitted --sps");
}

} // namespace

int main()
{
	try {
		command_hierarchy();
		option_parsing();
		help_and_errors();
		completion();
		std::cout << "CLI tests passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "CLI test failed: " << error.what() << '\n';
		return 1;
	}
}
