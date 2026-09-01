#include "core/cli.hpp"
#include "core/result_output.hpp"

#include "msap1/acquisition/ipc/acquisition_ipc.hpp"
#include "msap1/waveform/mncwf_v4_export.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace msap1::cli {
namespace {

struct WaveformResult {
	WaveformStatus status;
	std::vector<WaveformSessionIpc> sessions;
	std::vector<std::string> export_formats{"mncwf"};
};

struct WaveformExportResult {
	std::uint64_t session_id = 0;
	std::string event_id;
	std::string format;
	std::string file;
	std::uint64_t bytes = 0;
	std::string capture_uuid;
	std::uint64_t first_sequence = 0;
	std::uint64_t last_sequence = 0;
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

void require_ok(AcquisitionStatus status)
{
	if (status != AcquisitionStatus::ok)
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

std::string archive_state_name(WaveformArchiveDiscoveryState state)
{
	switch (state) {
	case WaveformArchiveDiscoveryState::not_started: return "not started";
	case WaveformArchiveDiscoveryState::scanning: return "scanning";
	case WaveformArchiveDiscoveryState::complete: return "complete";
	case WaveformArchiveDiscoveryState::cancelled: return "cancelled";
	case WaveformArchiveDiscoveryState::failed: return "failed";
	}
	return "unknown";
}

WaveformResult collect(WaveformResponse response)
{
	return {response.waveform, std::move(response.sessions), {"mncwf"}};
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
		       << "  Transport ring:       "
		       << status.transport_ring_blocks << " DMA blocks\n"
		       << "  DMA blocks:           " << status.blocks << '\n'
		       << "  History frames:       " << status.frames << '\n'
		       << "  History range:        " << status.history_oldest_sequence
		       << ".." << status.history_latest_sequence << '\n'
		       << "  History capacity:     " << status.history_capacity_frames
		       << " frames (128 MiB)\n"
		       << "  Invalid blocks:       " << status.invalid_blocks << '\n'
		       << "  Sequence gaps:        " << status.sequence_gaps << '\n'
		       << "  Transport overruns:   "
		       << status.transport_overrun_blocks
		       << " lapped-ring events (not lost blocks)\n"
		       << "  PL dropped frames:    " << status.pl_dropped_frames
		       << '\n'
		       << "  Capture budget:       " << status.max_capture_frames
		       << " frames pre+post\n"
		       << "  File write failures:  "
		       << status.materialization_failures << '\n'
		       << "  Completed sessions:   " << status.completed_sessions
		       << '\n'
		       << "  Incomplete sessions:  " << status.incomplete_sessions
		       << '\n'
		       << "  Archive discovery:    "
		       << archive_state_name(status.archive_discovery.state) << " ("
		       << status.archive_discovery.scanned_files << "/"
		       << status.archive_discovery.total_files << " scanned, "
		       << status.archive_discovery.rejected_files << " rejected)"
		       << "\n  Export formats:       mncwf\n";
		if (!result.sessions.empty()) {
			output << "\nRecent sessions\n";
			for (const auto &session : result.sessions) {
				output << "  " << session.id << "  "
				       << state_name(session.state) << "  seq "
				       << session.first_sequence << ".."
				       << session.last_sequence << "  events "
				       << session.event_count;
				if (!session.filename.empty())
					output << "  " << session.filename;
				output << '\n';
			}
		}
		return 0;
	}
};

class WaveformExportTextGenerator final
	: public ResultGenerator<WaveformExportResult> {
public:
	int write(const WaveformExportResult &result,
		std::ostream &output) const override
	{
		output << "MNCWF event export\n"
		       << "  Session:       " << result.session_id << '\n'
		       << "  Event:         " << result.event_id << '\n'
		       << "  File:          " << result.file << '\n'
		       << "  Bytes:         " << result.bytes << '\n'
		       << "  Capture UUID:  " << result.capture_uuid << '\n'
		       << "  Sample range:  " << result.first_sequence << ".."
		       << result.last_sequence << '\n';
		return 0;
	}
};

class WaveformExportJsonGenerator final
	: public ResultGenerator<WaveformExportResult> {
public:
	int write(const WaveformExportResult &result,
		std::ostream &output) const override
	{
		write_json_success(output, result);
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

int render(const Options &options, WaveformResponse response,
	   std::ostream &output)
{
	require_ok(response.status);
	return render_result(options, collect(std::move(response)), output,
			     WaveformTextGenerator{}, WaveformJsonGenerator{});
}

int run_status(const Options &options, std::ostream &output)
{
	AcquisitionClient client(options.socket_path);
	return render(options,
		      client.request(WaveformStatusRequest{}, options.timeout_ms),
		      output);
}

int run_list(const Options &options, std::ostream &output)
{
	AcquisitionClient client(options.socket_path);
	return render(options,
		      client.request(WaveformListRequest{}, options.timeout_ms),
		      output);
}

int run_trigger(const Options &options, std::ostream &output)
{
	AcquisitionClient client(options.socket_path);
	WaveformTriggerRequest trigger;
	trigger.pretrigger_ms = options.waveform_pretrigger_ms;
	trigger.posttrigger_ms = options.waveform_posttrigger_ms;
	trigger.decimation = options.waveform_decimation;
	trigger.source = WaveformTriggerSource::manual_cli;
	return render(options, client.request(trigger, options.timeout_ms),
		      output);
}

void write_export_file(const std::filesystem::path &path,
	const MncwfV4ExportFile &source)
{
	if (path.empty())
		throw std::invalid_argument("--file must not be empty");
	const int descriptor = ::open(path.c_str(),
		O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
	if (descriptor < 0) {
		if (errno == EEXIST)
			throw std::invalid_argument(
				"export file already exists: " + path.string());
		throw std::system_error(errno, std::generic_category(),
			"create waveform export " + path.string());
	}
	bool complete = false;
	try {
		std::array<std::byte, 64u * 1024u> buffer{};
		std::uint64_t offset = 0u;
		while (offset < source.size()) {
			const auto produced = source.read(offset, buffer);
			if (produced == 0u)
				throw std::runtime_error(
					"MNCWF export stopped before its declared size");
			std::size_t consumed = 0u;
			while (consumed < produced) {
				const auto written = ::write(descriptor,
					buffer.data() + consumed, produced - consumed);
				if (written < 0 && errno == EINTR)
					continue;
				if (written <= 0)
					throw std::system_error(errno,
						std::generic_category(),
						"write waveform export " + path.string());
				consumed += static_cast<std::size_t>(written);
			}
			offset += produced;
		}
		if (::fsync(descriptor) != 0)
			throw std::system_error(errno, std::generic_category(),
				"sync waveform export " + path.string());
		if (::close(descriptor) != 0)
			throw std::system_error(errno, std::generic_category(),
				"close waveform export " + path.string());
		complete = true;
	} catch (...) {
		if (!complete) {
			::close(descriptor);
			::unlink(path.c_str());
		}
		throw;
	}
}

int run_export(const Options &options, std::ostream &output)
{
	if (!options.waveform_session_id)
		throw std::invalid_argument("--session is required");
	if (!options.waveform_event_id)
		throw std::invalid_argument("--event is required");
	if (!options.waveform_export_format)
		throw std::invalid_argument("--format is required");
	if (*options.waveform_export_format != "mncwf")
		throw std::invalid_argument(
			"unsupported waveform export format; available formats: mncwf");
	const auto event_uuid =
		mncwf_uuid_from_string(*options.waveform_event_id);
	if (!event_uuid || mncwf_uuid_is_zero(*event_uuid))
		throw std::invalid_argument(
			"--event must be a nonzero canonical UUID");

	AcquisitionClient client(options.socket_path);
	auto response = client.request(WaveformListRequest{}, options.timeout_ms);
	require_ok(response.status);
	const auto session = std::ranges::find_if(response.sessions,
		[&options](const auto &candidate) {
			return candidate.id == *options.waveform_session_id;
		});
	if (session == response.sessions.end())
		throw std::invalid_argument("waveform session was not found");
	if (session->state != WaveformSessionState::complete ||
	    session->filename.empty())
		throw std::invalid_argument(
			"waveform session is not a completed capture");
	if (response.waveform_directory.empty())
		throw std::runtime_error(
			"acquisition daemon returned no waveform directory");

	const auto event_text = mncwf_uuid_string(*event_uuid);
	const auto default_name = "waveform-" +
		std::to_string(*options.waveform_session_id) + "-event-" +
		event_text + ".mncwf";
	const std::filesystem::path destination = options.waveform_export_file
		? std::filesystem::path(*options.waveform_export_file)
		: std::filesystem::path(default_name);
	const auto export_file = MncwfV4ExportFile::open(
		response.waveform_directory, session->filename, *event_uuid);
	write_export_file(destination, *export_file);

	const WaveformExportResult result{
		*options.waveform_session_id,
		event_text,
		"mncwf",
		destination.string(),
		export_file->size(),
		mncwf_uuid_string(export_file->capture_uuid()),
		export_file->first_sequence(),
		export_file->last_sequence(),
	};
	return render_result(options, result, output,
		WaveformExportTextGenerator{}, WaveformExportJsonGenerator{});
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
		"pre-ms", "MS", "History before the trigger (default: committed setting)",
		CompletionKind::none,
		[](Options &options, const std::string &value) {
			options.waveform_pretrigger_ms =
				parse_duration_ms(value, "--pre-ms");
		},
	});
	trigger.add_option({
		"post-ms", "MS", "Capture after the trigger (default: committed setting)",
		CompletionKind::none,
		[](Options &options, const std::string &value) {
			options.waveform_posttrigger_ms =
				parse_duration_ms(value, "--post-ms");
		},
	});
	trigger.add_option({
		"decimation", "N",
		"Store the mean of every N frames: 1, 2, 4, 8, 16, or 32 "
		"(default: committed setting)",
		CompletionKind::none,
		[](Options &options, const std::string &value) {
			std::size_t end = 0;
			unsigned long parsed = 0;
			try {
				parsed = std::stoul(value, &end, 0);
			} catch (const std::exception &) {
				throw std::invalid_argument(
					"--decimation requires a divisor");
			}
			if (end != value.size() ||
			    (parsed != 1u && parsed != 2u && parsed != 4u &&
			     parsed != 8u && parsed != 16u && parsed != 32u))
				throw std::invalid_argument(
					"--decimation must be 1, 2, 4, 8, 16, or 32");
			options.waveform_decimation =
				static_cast<std::uint32_t>(parsed);
		},
	});
	return trigger;
}

Command export_command()
{
	Command command(
		"export", "Export one event as a virtual MNCWF v4 capture",
		run_export,
		{
			.access = AccessLevel::local_only,
			.side_effect = SideEffect::control,
			.supports_text = true,
			.supports_json = true,
			.variants = {},
		});
	command.add_option({
		"session", "ID", "Completed waveform session ID",
		CompletionKind::none,
		[](Options &options, const std::string &value) {
			if (value.empty() || value.front() < '0' || value.front() > '9')
				throw std::invalid_argument(
					"--session requires a positive integer");
			std::size_t end = 0u;
			std::uint64_t parsed = 0u;
			try {
				parsed = std::stoull(value, &end, 0);
			} catch (const std::exception &) {
				throw std::invalid_argument(
					"--session requires a positive integer");
			}
			if (end != value.size() || parsed == 0u)
				throw std::invalid_argument(
					"--session requires a positive integer");
			options.waveform_session_id = parsed;
		},
	});
	command.add_option({
		"event", "UUID", "Canonical event UUID from the event catalogue",
		CompletionKind::none,
		[](Options &options, const std::string &value) {
			options.waveform_event_id = value;
		},
	});
	command.add_option({
		"format", "FORMAT", "Export format (available: mncwf)",
		CompletionKind::none,
		[](Options &options, const std::string &value) {
			if (value != "mncwf")
				throw std::invalid_argument(
					"--format must be mncwf; COMTRADE and PQDIF are not available");
			options.waveform_export_format = value;
		},
	});
	command.add_option({
		"file", "PATH", "Destination (default: generated .mncwf name)",
		CompletionKind::path,
		[](Options &options, const std::string &value) {
			if (value.empty())
				throw std::invalid_argument("--file must not be empty");
			options.waveform_export_file = value;
		},
	});
	return command;
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
	waveform.add_subcommand(export_command());
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
