#include "core/cli.hpp"
#include "core/result_output.hpp"

#include "mnc/waveform/waveform_converter.hpp"
#include "msap1/acquisition/ipc/acquisition_ipc.hpp"
#include "msap1/waveform/mncwf_v4_export.hpp"
#include "msap1/waveform/mncwf_waveform_source.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <optional>
#include <ostream>
#include <sstream>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <openssl/evp.h>
#include <sys/stat.h>
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
	std::string scope;
	std::string event_id;
	std::string format;
	std::string profile;
	std::string file;
	std::uint64_t bytes = 0;
	std::string sha256;
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

std::string compression_name(WaveformCompression compression)
{
	switch (compression) {
	case WaveformCompression::none: return "pending/none";
	case WaveformCompression::zstd_chunks: return "zstd chunks";
	case WaveformCompression::mixed_raw_zstd_chunks:
		return "mixed raw/zstd chunks";
	case WaveformCompression::raw_chunks: return "raw chunks";
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
		       << "  Archive use/limit:    " << status.archive_stored_bytes
		       << "/" << status.archive_limit_bytes << " bytes\n"
		       << "  Expired sessions:     " << status.expired_sessions << '\n'
		       << "  Retention failures:   " << status.retention_failures << '\n'
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
				output << "  v" << session.format_version << "  "
				       << compression_name(session.compression) << "  "
				       << session.stored_bytes << "/"
				       << session.logical_sample_bytes
				       << " stored/logical bytes";
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
		output << "Waveform export\n"
		       << "  Session:       " << result.session_id << '\n'
		       << "  Scope:         " << result.scope << '\n';
		if (!result.event_id.empty())
			output << "  Event:         " << result.event_id << '\n';
		output << "  Format:        " << result.format << '\n'
		       << "  Profile:       " << result.profile << '\n';
		output << "  File:          " << result.file << '\n'
		       << "  Bytes:         " << result.bytes << '\n'
		       << "  SHA-256:       " << result.sha256 << '\n';
		if (!result.capture_uuid.empty())
			output << "  Capture UUID:  " << result.capture_uuid << '\n'
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

class Descriptor final {
public:
	explicit Descriptor(int value = -1) : value_(value) {}
	~Descriptor() { if (value_ >= 0) ::close(value_); }
	Descriptor(const Descriptor &) = delete;
	Descriptor &operator=(const Descriptor &) = delete;
	Descriptor(Descriptor &&other) noexcept
		: value_(std::exchange(other.value_, -1)) {}
	Descriptor &operator=(Descriptor &&other) noexcept
	{
		if (this != &other) {
			if (value_ >= 0)
				::close(value_);
			value_ = std::exchange(other.value_, -1);
		}
		return *this;
	}
	[[nodiscard]] int get() const noexcept { return value_; }

private:
	int value_;
};

struct WrittenExport {
	std::uint64_t bytes;
	std::string sha256;
};

std::string digest_text(std::span<const unsigned char> digest)
{
	std::ostringstream output;
	output << std::hex << std::setfill('0');
	for (const auto value : digest)
		output << std::setw(2) << static_cast<unsigned>(value);
	return output.str();
}

class ExclusiveExport final : public mnc::waveform::OutputSink {
public:
	explicit ExclusiveExport(std::filesystem::path path)
		: path_(std::move(path)), digest_(EVP_MD_CTX_new(), EVP_MD_CTX_free)
	{
		if (path_.empty())
			throw std::invalid_argument("--file must not be empty");
		if (!digest_ || EVP_DigestInit_ex(digest_.get(), EVP_sha256(), nullptr) != 1)
			throw std::runtime_error("initialize waveform export SHA-256");
		descriptor_ = ::open(path_.c_str(),
			O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
		if (descriptor_ < 0) {
			if (errno == EEXIST)
				throw std::invalid_argument(
					"export file already exists: " + path_.string());
			throw std::system_error(errno, std::generic_category(),
				"create waveform export " + path_.string());
		}
	}

	~ExclusiveExport()
	{
		if (descriptor_ >= 0)
			::close(descriptor_);
		if (!complete_)
			::unlink(path_.c_str());
	}

	void write(std::span<const std::byte> bytes) override
	{
		if (bytes.size() > byte_limit() - bytes_)
			throw std::runtime_error(
				"waveform export exceeds the 1 GiB output limit");
		if (EVP_DigestUpdate(digest_.get(), bytes.data(), bytes.size()) != 1)
			throw std::runtime_error("update waveform export SHA-256");
		std::size_t consumed = 0;
		while (consumed < bytes.size()) {
			const auto count = ::write(descriptor_, bytes.data() + consumed,
				bytes.size() - consumed);
			if (count < 0 && errno == EINTR)
				continue;
			if (count <= 0)
				throw std::system_error(errno, std::generic_category(),
					"write waveform export " + path_.string());
			consumed += static_cast<std::size_t>(count);
		}
		bytes_ += bytes.size();
	}

	[[nodiscard]] std::uint64_t bytes_written() const noexcept override
	{
		return bytes_;
	}

	[[nodiscard]] std::uint64_t byte_limit() const noexcept override
	{
		return 1024ull * 1024ull * 1024ull;
	}

	WrittenExport finish(std::optional<std::string_view> expected_sha256 = {})
	{
		if (::fsync(descriptor_) != 0)
			throw std::system_error(errno, std::generic_category(),
				"sync waveform export " + path_.string());
		std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
		unsigned int digest_size = 0;
		if (EVP_DigestFinal_ex(digest_.get(), digest.data(), &digest_size) != 1)
			throw std::runtime_error("finish waveform export SHA-256");
		const auto descriptor = std::exchange(descriptor_, -1);
		if (::close(descriptor) != 0)
			throw std::system_error(errno, std::generic_category(),
				"close waveform export " + path_.string());
		WrittenExport result{bytes_, digest_text(
			std::span(digest.data(), static_cast<std::size_t>(digest_size)))};
		if (expected_sha256 && result.sha256 != *expected_sha256)
			throw std::runtime_error(
				"downloaded waveform export failed SHA-256 verification");
		complete_ = true;
		return result;
	}

private:
	std::filesystem::path path_;
	int descriptor_ = -1;
	std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> digest_;
	std::uint64_t bytes_ = 0;
	bool complete_ = false;
};

template<class Reader>
WrittenExport write_export_file(const std::filesystem::path &path,
	std::uint64_t size, Reader &&reader,
	std::optional<std::string_view> expected_sha256 = {})
{
	ExclusiveExport output(path);
	std::array<std::byte, 64u * 1024u> buffer{};
	std::uint64_t offset = 0;
	while (offset < size) {
		const auto requested = static_cast<std::size_t>(
			std::min<std::uint64_t>(buffer.size(), size - offset));
		const auto produced = reader(offset,
			std::span<std::byte>(buffer).first(requested));
		if (produced == 0u || produced > requested)
			throw std::runtime_error(
				"waveform export stopped before its declared size");
		output.write(std::span<const std::byte>(buffer).first(produced));
		offset += produced;
	}
	return output.finish(expected_sha256);
}

struct OpenedCapture {
	Descriptor descriptor;
	std::uint64_t size = 0;
};

OpenedCapture open_capture(std::string_view directory_name,
	std::string_view filename)
{
	const std::filesystem::path source_name(filename);
	if (source_name.empty() || source_name != source_name.filename() ||
	    source_name.extension() != ".mncwf")
		throw std::runtime_error(
			"acquisition daemon returned an invalid waveform filename");
	Descriptor directory(::open(std::string(directory_name).c_str(),
		O_RDONLY | O_CLOEXEC | O_DIRECTORY));
	if (directory.get() < 0)
		throw std::system_error(errno, std::generic_category(),
			"open waveform directory");
	Descriptor source(::openat(directory.get(), source_name.c_str(),
		O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
	if (source.get() < 0)
		throw std::system_error(errno, std::generic_category(),
			"open waveform capture");
	struct stat status {};
	if (::fstat(source.get(), &status) != 0 || !S_ISREG(status.st_mode) ||
	    status.st_size <= 0 || static_cast<std::uint64_t>(status.st_size) >
		mncwf_v4_max_file_bytes)
		throw std::runtime_error(
			"waveform capture is not a nonempty regular file");
	return {std::move(source), static_cast<std::uint64_t>(status.st_size)};
}

int run_export(const Options &options, std::ostream &output)
{
	if (!options.waveform_session_id)
		throw std::invalid_argument("--session is required");
	if (!options.waveform_export_format)
		throw std::invalid_argument("--format is required");
	const auto &format = *options.waveform_export_format;
	if (format != "mncwf" && format != "comtrade" &&
	    format != "comtrade-zip" && format != "pqdif")
		throw std::invalid_argument(
			"unsupported waveform export format; available formats: "
			"mncwf, comtrade, comtrade-zip, pqdif");
	std::optional<MncwfUuid> event_uuid;
	std::string event_text;
	if (options.waveform_event_id) {
		event_uuid = mncwf_uuid_from_string(*options.waveform_event_id);
		if (!event_uuid || mncwf_uuid_is_zero(*event_uuid))
			throw std::invalid_argument(
				"--event must be a nonzero canonical UUID");
		event_text = mncwf_uuid_string(*event_uuid);
	}

	AcquisitionClient client(options.socket_path);
	WaveformLookupRequest lookup;
	lookup.session_id = *options.waveform_session_id;
	auto response = client.request(lookup, options.timeout_ms);
	require_ok(response.status);
	if (!response.found)
		throw std::invalid_argument("waveform session was not found");
	const auto &session = response.session;
	if (session.state != WaveformSessionState::complete ||
	    session.filename.empty())
		throw std::invalid_argument(
			"waveform session is not a completed capture");
	if (response.waveform_directory.empty())
		throw std::runtime_error(
			"acquisition daemon returned no waveform directory");

	const auto scope = event_uuid ? "event" : "capture";
	WaveformExportResult result;
	result.session_id = *options.waveform_session_id;
	result.scope = scope;
	result.event_id = event_text;
	result.format = format;
	result.capture_uuid = session.capture_uuid;
	result.first_sequence = session.first_sequence;
	result.last_sequence = session.last_sequence;

	if (format == "mncwf") {
		const auto default_name = "waveform-" +
			std::to_string(*options.waveform_session_id) +
			(event_uuid ? "-event-" + event_text : std::string{}) + ".mncwf";
		const std::filesystem::path destination = options.waveform_export_file
			? std::filesystem::path(*options.waveform_export_file)
			: std::filesystem::path(default_name);
		WrittenExport written{};
		if (event_uuid) {
			const auto source = MncwfV4ExportFile::open(
				response.waveform_directory, session.filename, *event_uuid);
			written = write_export_file(destination, source->size(),
				[&source](auto offset, auto destination_buffer) {
					return source->read(offset, destination_buffer);
				});
			result.capture_uuid = mncwf_uuid_string(source->capture_uuid());
			result.first_sequence = source->first_sequence();
			result.last_sequence = source->last_sequence();
		} else {
			auto source = open_capture(response.waveform_directory,
				session.filename);
			written = write_export_file(destination, source.size,
				[&source](std::uint64_t offset,
					std::span<std::byte> destination_buffer) {
					for (;;) {
						const auto count = ::pread(source.descriptor.get(),
							destination_buffer.data(), destination_buffer.size(),
							static_cast<off_t>(offset));
						if (count < 0 && errno == EINTR) continue;
						if (count < 0)
							throw std::system_error(errno,
								std::generic_category(), "read waveform capture");
						return static_cast<std::size_t>(count);
					}
				});
		}
		result.profile = "MNCWF v4/v5";
		result.file = destination.string();
		result.bytes = written.bytes;
		result.sha256 = std::move(written.sha256);
	} else {
		const auto export_format = mnc::waveform::export_format_from_name(format);
		if (!export_format)
			throw std::logic_error("validated waveform format has no converter");
		const auto extension = *export_format ==
			mnc::waveform::ExportFormat::comtrade ? ".cff"
			: *export_format == mnc::waveform::ExportFormat::comtrade_zip
				? ".zip" : ".pqd";
		const auto default_name = "waveform-" +
			std::to_string(*options.waveform_session_id) +
			(event_uuid ? "-event-" + event_text : std::string{}) + extension;
		const std::filesystem::path destination = options.waveform_export_file
			? std::filesystem::path(*options.waveform_export_file)
			: std::filesystem::path(default_name);
		auto opened = open_capture(response.waveform_directory,
			session.filename);
		MncwfWaveformSource source(opened.descriptor.get(), *export_format,
			event_uuid ? mnc::waveform::ExportScope::event
				: mnc::waveform::ExportScope::capture,
			event_uuid);
		mnc::waveform::ConversionOptions conversion;
		conversion.format = *export_format;
		conversion.scope = event_uuid ? mnc::waveform::ExportScope::event
			: mnc::waveform::ExportScope::capture;
		conversion.selected_event_uuid = event_uuid;
		conversion.maximum_output_bytes = 1024ull * 1024ull * 1024ull;
		conversion.output_stem = destination.stem().string();
		std::stop_source cancellation;
		ExclusiveExport destination_file(destination);
		const auto converter = mnc::waveform::make_converter(*export_format);
		const auto summary = converter->convert(source, destination_file,
			conversion, cancellation.get_token(), [&](const auto &) {
				if (stop_was_requested())
					cancellation.request_stop();
			});
		if (stop_was_requested())
			throw std::runtime_error("waveform export cancelled");
		const auto written = destination_file.finish();
		if (written.bytes != summary.bytes)
			throw std::runtime_error(
				"waveform converter reported an inconsistent output size");
		result.profile = summary.profile;
		result.file = destination.string();
		result.bytes = written.bytes;
		result.sha256 = written.sha256;
		result.capture_uuid = mnc::waveform::uuid_string(
			source.metadata().capture.capture_uuid);
		result.first_sequence = source.metadata().first_sequence;
		result.last_sequence = source.metadata().last_sequence;
	}
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
		"export", "Export a complete capture or one exact event slice",
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
		"event", "UUID", "Optional canonical event UUID (default: full capture)",
		CompletionKind::none,
		[](Options &options, const std::string &value) {
			options.waveform_event_id = value;
		},
	});
	command.add_option({
		"format", "FORMAT",
		"Export format: mncwf, comtrade, comtrade-zip, or pqdif",
		CompletionKind::none,
		[](Options &options, const std::string &value) {
			if (value != "mncwf" && value != "comtrade" &&
			    value != "comtrade-zip" && value != "pqdif")
				throw std::invalid_argument(
					"--format must be mncwf, comtrade, comtrade-zip, or pqdif");
			options.waveform_export_format = value;
		},
	});
	command.add_option({
		"file", "PATH", "Exclusive destination (default: generated name)",
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
