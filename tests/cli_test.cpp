#include "core/cli.hpp"

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
	const auto temperature =
		application.parse({"help", "system", "temperature"});
	require(temperature.show_help &&
			temperature.command->name() == "temperature",
		"explicit help did not select the temperature command");
	const auto rpu = application.parse({"help", "rpu"});
	require(rpu.show_help && rpu.command->name() == "rpu",
		"explicit help did not select the RPU command");
}

void option_parsing()
{
	const auto application = msap1::cli::make_application();
	const auto invocation = application.parse({
		"--timeout-ms", "42", "meter", "view", "--results", "3",
		"--socket", "/tmp/acquisition.sock", "--duration=1.5",
		"--output", "text", "--help",
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
	require(invocation.options.output_format ==
			msap1::cli::OutputFormat::text,
		"output format was not parsed");

	const auto rate =
		application.parse({"adc", "rate", "--sps", "16000"});
	require(!rate.show_help && rate.command->name() == "rate",
		"ADC rate command was not selected");
	require(rate.options.sample_rate_hz == 16000,
		"ADC sample rate was not parsed");

	const auto health =
		application.parse({"meter", "health", "--refresh", "--full"});
	require(!health.show_help && health.command->name() == "health" &&
			health.options.health_refresh && health.options.health_full,
		"meter health diagnostic options were not parsed");

	const auto wiring = application.parse({
		"meter", "wiring", "set", "--preset", "acb",
		"--ch0-direction", "reversed", "--ch3-direction", "normal"});
	require(!wiring.show_help && wiring.command->name() == "set" &&
		wiring.options.current_wiring_preset == "acb" &&
		wiring.options.current_channel_direction[0] == "reversed" &&
		wiring.options.current_channel_direction[3] == "normal",
		"current wiring preset/direction options were not parsed");
	const auto custom_wiring = application.parse({
		"meter", "wiring", "set", "--ch0-phase", "c",
		"--ch1-phase", "a", "--ch2-phase", "n", "--ch3-phase", "b"});
	require(custom_wiring.options.current_channel_phase[0] == "c" &&
		custom_wiring.options.current_channel_phase[1] == "a" &&
		custom_wiring.options.current_channel_phase[2] == "n" &&
		custom_wiring.options.current_channel_phase[3] == "b",
		"explicit current channel phases were not parsed");

	const auto flow =
		application.parse({"adc", "testflw", "--flow", "1"});
	require(!flow.show_help && flow.command->name() == "testflw",
		"ADC diagnostic flow command was not selected");
	require(flow.options.diagnostic_flow == 1,
		"ADC diagnostic flow was not parsed");

	const auto waveform_export = application.parse({
		"waveform", "export", "--session", "17", "--event",
		"01234567-89ab-5def-8123-456789abcdef", "--format", "mncwf",
		"--file", "/tmp/event.mncwf"});
	require(!waveform_export.show_help &&
			waveform_export.command->name() == "export" &&
			waveform_export.options.waveform_session_id == 17u &&
			waveform_export.options.waveform_event_id ==
				"01234567-89ab-5def-8123-456789abcdef" &&
			waveform_export.options.waveform_export_format == "mncwf" &&
			waveform_export.options.waveform_export_file ==
				"/tmp/event.mncwf",
		"waveform export selection was not parsed");

	const auto pq_events = application.parse({
		"meter", "power-quality", "events", "--event",
		"01234567-89ab-5def-8123-456789abcdef", "--start-utc-ns",
		"-100", "--end-utc-ns", "200", "--limit", "7"});
	require(!pq_events.show_help && pq_events.command->name() == "events" &&
			pq_events.options.meter_event_id ==
				"01234567-89ab-5def-8123-456789abcdef" &&
			pq_events.options.meter_event_start_utc_ns == -100 &&
			pq_events.options.meter_event_end_utc_ns == 200 &&
			pq_events.options.result_limit == 7,
		"power-quality event query options were not parsed");

	const auto log = application.parse({
		"log", "--component", "fpga-acquisition", "--module=dma",
		"--priority", "warning", "--since", "10 minutes ago",
		"--limit", "25", "--follow", "--json",
	});
	require(!log.show_help && log.command->name() == "log",
		"log command was not selected");
	require(log.options.log_component == "fpga-acquisition" &&
			log.options.log_module == "dma",
		"log classification filters were not parsed");
	require(log.options.log_priority == "warning" &&
			log.options.log_since == "10 minutes ago",
		"log priority/time filters were not parsed");
	require(log.options.result_limit == 25 && log.options.log_follow &&
			log.options.log_json,
		"log limit/flag options were not parsed");
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
	output.str({});
	error.str({});
	require(application.execute(
		{"adc", "testflw", "--flow", "2"}, output, error) == 2,
		"unsupported ADC diagnostic flow was accepted");
	require(error.str().find("supports only flow 1") != std::string::npos,
		"diagnostic-flow error omitted the supported flow");
	output.str({});
	error.str({});
	require(application.execute(
		{"waveform", "export", "--session", "1", "--event",
		 "01234567-89ab-5def-8123-456789abcdef", "--format", "pqdif"},
		output, error) == 2,
		"unavailable PQDIF export format was accepted");
	require(error.str().find("COMTRADE and PQDIF are not available") !=
			std::string::npos,
		"unavailable converter error omitted the explicit scope boundary");
	output.str({});
	error.str({});
	require(application.execute(
		{"meter", "power-quality", "events", "--event", "not-a-uuid"},
		output, error) == 2,
		"invalid power-quality event UUID was accepted");
	output.str({});
	error.str({});
	require(application.execute(
		{"log", "--priority", "verbose"}, output, error) == 2,
		"unknown log priority was accepted");
	output.str({});
	error.str({});
	require(application.execute(
		{"log", "--follow=true"}, output, error) == 2,
		"flag option accepted an inline value");
	output.str({});
	error.str({});
	require(application.execute(
		{"meter", "wiring", "set", "--preset", "abc", "--ch0-phase", "a"},
		output, error) == 2,
		"current wiring accepted a preset with explicit phases");

	output.str({});
	error.str({});
	require(application.execute(
		{"--output", "json", "meter", "unknown"}, output, error) == 2,
		"machine usage error did not return status 2");
	require(output.str().find(R"("success":false)") != std::string::npos &&
			output.str().find(R"("code":"USAGE_ERROR")") !=
				std::string::npos &&
			error.str().empty(),
		"machine usage error was not a pure JSON envelope");
}

void machine_interface()
{
	const auto application = msap1::cli::make_application();
	const auto descriptors = application.descriptors();
	const auto find = [&descriptors](const std::string &path) {
		return std::find_if(descriptors.begin(), descriptors.end(),
			[&path](const auto &descriptor) {
				return descriptor.command == path;
			});
	};
	const auto health = find("mnc meter health");
	require(health != descriptors.end() &&
			health->metadata.access ==
				msap1::cli::AccessLevel::diagnostic &&
			health->metadata.supports_json,
		"meter health metadata is not diagnostic JSON");
	const auto rpu = find("mnc rpu");
	require(rpu != descriptors.end() &&
			rpu->metadata.access ==
				msap1::cli::AccessLevel::diagnostic &&
			rpu->metadata.supports_json,
		"RPU metadata is not diagnostic JSON");
	const auto start = find("mnc adc start");
	require(start != descriptors.end() &&
			start->metadata.access ==
				msap1::cli::AccessLevel::operator_control,
		"ADC start metadata is not operator control");
	const auto view = find("mnc meter view");
	require(view != descriptors.end() &&
			view->metadata.access ==
				msap1::cli::AccessLevel::local_only,
		"meter view metadata is not local-only");
	const auto wiring_show = find("mnc meter wiring show");
	const auto wiring_set = find("mnc meter wiring set");
	require(wiring_show != descriptors.end() && wiring_set != descriptors.end() &&
		wiring_show->metadata.access ==
			msap1::cli::AccessLevel::diagnostic &&
		wiring_set->metadata.access ==
			msap1::cli::AccessLevel::operator_control &&
		wiring_set->metadata.side_effect == msap1::cli::SideEffect::control,
		"current wiring command metadata is incorrect");
	const auto waveform_export = find("mnc waveform export");
	require(waveform_export != descriptors.end() &&
			waveform_export->metadata.access ==
				msap1::cli::AccessLevel::local_only &&
			waveform_export->metadata.side_effect ==
				msap1::cli::SideEffect::control &&
			waveform_export->metadata.supports_json,
		"waveform export metadata does not prevent remote file writes");
	const auto pq_events = find("mnc meter power-quality events");
	const auto flicker = find("mnc meter flicker");
	const auto mains = find("mnc meter mains-signalling");
	require(pq_events != descriptors.end() && flicker != descriptors.end() &&
			mains != descriptors.end() &&
			pq_events->metadata.access ==
				msap1::cli::AccessLevel::diagnostic &&
			flicker->metadata.supports_json && mains->metadata.supports_json,
		"M18 typed meter commands are not diagnostic JSON commands");

	const msap1::cli::ExecutionPolicy restricted{
		.maximum_access = msap1::cli::AccessLevel::diagnostic,
		.require_json = true,
		.allow_socket_override = false,
		.allow_timeout_override = false,
	};
	std::ostringstream output;
	std::ostringstream error;
	require(application.execute(
			{"--output", "json", "machine", "describe"},
			output, error, restricted) == 0,
		"restricted machine describe failed");
	require(output.str().find(R"("interface_version":1)") !=
			std::string::npos,
		"machine describe omitted the interface version");

	output.str({});
	error.str({});
	require(application.execute(
			{"--output", "json", "adc", "start"}, output, error,
			restricted) == 3,
		"restricted ADC start was not denied");
	require(output.str().find(R"("code":"ACCESS_DENIED")") !=
			std::string::npos,
		"restricted denial omitted its JSON error code");

	output.str({});
	error.str({});
	require(application.execute(
			{"--output", "json", "--socket", "/tmp/other",
			 "machine", "describe"},
			output, error, restricted) == 3,
		"restricted socket override was accepted");
	require(output.str().find(R"("code":"ACCESS_DENIED")") !=
			std::string::npos,
		"restricted socket override omitted its authorization error");

	output.str({});
	error.str({});
	require(application.execute(
			{"machine", "describe"}, output, error, restricted) == 2,
		"restricted execution accepted non-JSON output");
	require(output.str().find(R"("code":"OUTPUT_REQUIRED")") !=
			std::string::npos,
		"restricted non-JSON request omitted its output error");

	output.str({});
	error.str({});
	require(application.execute(
			{"--output", "json", "--socket",
			 "/tmp/mnc-cli-test-missing.sock", "meter", "health"},
			output, error) == 4,
		"missing acquisition service did not return unavailable status");
	require(output.str().find(R"("code":"SERVICE_UNAVAILABLE")") !=
			std::string::npos,
		"unavailable acquisition service omitted its JSON error code");
}

void completion()
{
	const auto application = msap1::cli::make_application();
	auto candidates = application.complete({""});
	require(contains(candidates, "adc") && contains(candidates, "meter") &&
			contains(candidates, "log") && contains(candidates, "system") &&
			contains(candidates, "machine"),
		"root completion omitted command groups");
	candidates = application.complete({"meter", ""});
	require(contains(candidates, "health") && contains(candidates, "view") &&
			contains(candidates, "snapshot") && contains(candidates, "wiring"),
		"meter completion omitted actions");
	candidates = application.complete({"meter", "wiring", ""});
	require(contains(candidates, "show") && contains(candidates, "set"),
		"current wiring completion omitted actions");
	candidates = application.complete({"meter", "wiring", "set", "--"});
	require(contains(candidates, "--preset") &&
		contains(candidates, "--ch0-phase") &&
		contains(candidates, "--ch3-direction"),
		"current wiring completion omitted configuration options");
	candidates = application.complete({"meter", "health", "--"});
	require(contains(candidates, "--refresh") &&
			contains(candidates, "--full"),
		"meter health completion omitted diagnostic options");
	candidates = application.complete({"meter", "power-quality", ""});
	require(contains(candidates, "events"),
		"power-quality completion omitted durable events");
	candidates = application.complete(
		{"meter", "power-quality", "events", "--"});
	require(contains(candidates, "--event") &&
			contains(candidates, "--start-utc-ns") &&
			contains(candidates, "--end-utc-ns") &&
			contains(candidates, "--limit"),
		"power-quality event completion omitted query options");
	candidates = application.complete({"meter", "view", "--"});
	require(contains(candidates, "--duration") &&
		contains(candidates, "--results") && contains(candidates, "--socket"),
		"leaf completion omitted options");
	candidates = application.complete({"meter", "view", "--socket", ""});
	require(candidates == std::vector<std::string>{"__MNC_FILE__"},
		"socket completion did not request path completion");
	candidates = application.complete({"adc", ""});
	require(contains(candidates, "rate") && contains(candidates, "start") &&
		contains(candidates, "stop") && contains(candidates, "testflw"),
		"ADC completion omitted actions");
	candidates = application.complete({"adc", "rate", "--"});
	require(contains(candidates, "--sps"),
		"ADC rate completion omitted --sps");
	candidates = application.complete({"adc", "testflw", "--"});
	require(contains(candidates, "--flow"),
		"ADC diagnostic completion omitted --flow");
	candidates = application.complete({"waveform", "export", "--"});
	require(contains(candidates, "--session") &&
			contains(candidates, "--event") &&
			contains(candidates, "--format") &&
			contains(candidates, "--file"),
		"waveform export completion omitted required options");
	candidates = application.complete({"log", "--"});
	require(contains(candidates, "--component") &&
			contains(candidates, "--follow") &&
			contains(candidates, "--json") &&
			contains(candidates, "--cursor"),
		"log completion omitted options");
	candidates = application.complete({"system", ""});
	require(contains(candidates, "temperature"),
		"system completion omitted temperature");
}

} // namespace

int main()
{
	try {
		command_hierarchy();
		option_parsing();
		help_and_errors();
		machine_interface();
		completion();
		std::cout << "CLI tests passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "CLI test failed: " << error.what() << '\n';
		return 1;
	}
}
