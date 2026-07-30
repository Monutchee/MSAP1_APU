#include "cli.hpp"
#include "result_output.hpp"

#include "msap1/acquisition_ipc.hpp"

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace msap1::cli {
namespace {

struct WaveformResult {
	WaveformStatus status;
	std::vector<WaveformSessionSummary> sessions;
};

std::uint32_t parse_duration_ms(const std::string &value,
				const std::string &option)
{
	std::size_t end = 0;
	unsigned long parsed = 0;
	try {
		parsed = std::stoul(value, &end, 0);
	} catch (const std::exception &) {
		throw std::invalid_argument(option + " requires milliseconds");
	}
	if (end != value.size() || parsed > 120000u)
		throw std::invalid_argument(option + " must be 0..120000 ms");
	return static_cast<std::uint32_t>(parsed);
}

void require_ok(const AcquisitionResponse &response)
{
	if (response.status != AcquisitionStatus::ok)
		throw std::runtime_error("acquisition daemon rejected waveform request");
}

std::string state_name(WaveformSessionState state)
{
	switch (state) {
	case WaveformSessionState::capturing: return "capturing";
	case WaveformSessionState::complete: return "complete";
	case WaveformSessionState::incomplete: return "incomplete";
	}
	return "unknown";
}

WaveformResult collect(const AcquisitionResponse &response)
{
	WaveformResult result{response.waveform, {}};
	const auto count = std::min<std::size_t>(
		response.waveform_session_count,
		response.waveform_sessions.size());
	result.sessions.assign(response.waveform_sessions.begin(),
			       response.waveform_sessions.begin() + count);
	return result;
}

class WaveformTextGenerator final : public ResultGenerator<WaveformResult> {
public:
	int write(const WaveformResult &result, std::ostream &output) const override
	{
		const auto &status = result.status;
		output << "MSAP1 waveform capture\n"
		       << "  DMA running:          "
		       << (status.running ? "yes" : "no") << '\n'
		       << "  Active session:       "
		       << (status.active_session ? "yes" : "no") << '\n'
		       << "  Sample rate:          " << status.sample_rate_hz
		       << " frame/s\n"
		       << "  DMA blocks:           " << status.blocks << '\n'
		       << "  History frames:       " << status.frames << '\n'
		       << "  History range:        " << status.history_oldest_sequence
		       << ".." << status.history_latest_sequence << '\n'
		       << "  History capacity:     " << status.history_capacity_frames
		       << " frames (128 MiB)\n"
		       << "  Invalid blocks:       " << status.invalid_blocks << '\n'
		       << "  Sequence gaps:        " << status.sequence_gaps << '\n'
		       << "  Completed sessions:   " << status.completed_sessions
		       << '\n'
		       << "  Incomplete sessions:  " << status.incomplete_sessions
		       << '\n';
		if (!result.sessions.empty()) {
			output << "\nRecent sessions\n";
			for (const auto &session : result.sessions) {
				output << "  " << session.id << "  "
				       << state_name(session.state) << "  seq "
				       << session.first_sequence << ".."
				       << session.last_sequence << "  events "
				       << session.event_count;
				if (session.filename.front() != '\0')
					output << "  " << session.filename.data();
				output << '\n';
			}
		}
		return 0;
	}
};

class WaveformJsonGenerator final : public ResultGenerator<WaveformResult> {
public:
	int write(const WaveformResult &result, std::ostream &output) const override
	{
		write_json_success(output, result);
		return 0;
	}
};

int render(const Options &options, const AcquisitionResponse &response,
	   std::ostream &output)
{
	require_ok(response);
	return render_result(options, collect(response), output,
			     WaveformTextGenerator{}, WaveformJsonGenerator{});
}

int run_status(const Options &options, std::ostream &output)
{
	AcquisitionClient client(options.socket_path);
	return render(options,
		      client.request(AcquisitionCommand::waveform_status,
				     options.timeout_ms),
		      output);
}

int run_list(const Options &options, std::ostream &output)
{
	AcquisitionClient client(options.socket_path);
	return render(options,
		      client.request(AcquisitionCommand::waveform_list,
				     options.timeout_ms),
		      output);
}

int run_trigger(const Options &options, std::ostream &output)
{
	AcquisitionClient client(options.socket_path);
	return render(options,
		      client.request(AcquisitionCommand::waveform_trigger,
				     options.timeout_ms, nullptr, 0, 0,
				     options.waveform_pretrigger_ms,
				     options.waveform_posttrigger_ms,
				     WaveformTriggerSource::manual_cli),
		      output);
}

Command trigger_command()
{
	Command trigger(
		"trigger", "Trigger a pre/post waveform capture", run_trigger,
		{
			.access = AccessLevel::operator_control,
			.side_effect = SideEffect::control,
			.supports_text = true,
			.supports_json = true,
			.variants = {},
		});
	trigger.add_option({
		"pre-ms", "MS", "History before the trigger (default: 10000)",
		CompletionKind::none,
		[](Options &options, const std::string &value) {
			options.waveform_pretrigger_ms =
				parse_duration_ms(value, "--pre-ms");
		},
	});
	trigger.add_option({
		"post-ms", "MS", "Capture after the trigger (default: 10000)",
		CompletionKind::none,
		[](Options &options, const std::string &value) {
			options.waveform_posttrigger_ms =
				parse_duration_ms(value, "--post-ms");
		},
	});
	return trigger;
}

} // namespace

void register_waveform_commands(Application &application)
{
	Command waveform("waveform", "Inspect and trigger raw waveform capture");
	waveform.add_subcommand(Command(
		"status", "Show waveform DMA and history status", run_status,
		{
			.access = AccessLevel::diagnostic,
			.side_effect = SideEffect::none,
			.supports_text = true,
			.supports_json = true,
			.variants = {},
		}));
	waveform.add_subcommand(trigger_command());
	waveform.add_subcommand(Command(
		"list", "List recent waveform capture sessions", run_list,
		{
			.access = AccessLevel::diagnostic,
			.side_effect = SideEffect::none,
			.supports_text = true,
			.supports_json = true,
			.variants = {},
		}));
	application.add_command(std::move(waveform));
}

} // namespace msap1::cli
