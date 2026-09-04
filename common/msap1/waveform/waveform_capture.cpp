#include "msap1/waveform/waveform_capture.hpp"

#include "mnc/logging/logging.hpp"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <cstring>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

namespace msap1 {
namespace {

constexpr unsigned long waveform_transport_status_ioctl =
	_IOR('W', 0x02, WaveformTransportStatusIoctl);

/*
 * Session outcomes are journaled here, where the state transitions happen:
 * half of them (gap intersection, epoch reset) never reach the writer, so
 * they otherwise leave no trace anywhere — a capture could fail with every
 * failure counter still reading zero.
 */
const mnc::logging::Logger capture_log{"fpga-acquisition", "waveform"};

[[noreturn]] void throw_errno(const std::string &operation);

void set_current_thread_nice(int value, std::string_view role) noexcept
{
	if (::setpriority(PRIO_PROCESS, 0, value) == 0)
		return;
	const std::array fields{
		mnc::logging::Field{"MNC_THREAD_ROLE", std::string(role)},
		mnc::logging::Field{"MNC_EXPECTED_NICE", std::to_string(value)},
		mnc::logging::Field{"MNC_ERRNO", std::to_string(errno)}};
	(void)capture_log.write(mnc::logging::Priority::warning,
		"could not apply waveform worker thread priority",
		"waveform_thread_priority_failed", fields);
}

class MappedFile {
public:
	explicit MappedFile(const std::filesystem::path &path)
	{
		fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
		if (fd_ < 0)
			throw_errno("open " + path.string());
		struct stat status {};
		if (::fstat(fd_, &status) != 0) {
			const auto saved = errno;
			::close(fd_);
			fd_ = -1;
			errno = saved;
			throw_errno("stat " + path.string());
		}
		if (status.st_size <= 0) {
			::close(fd_);
			fd_ = -1;
			throw std::invalid_argument("waveform file is empty");
		}
		size_ = static_cast<std::size_t>(status.st_size);
		mapping_ = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
		if (mapping_ == MAP_FAILED) {
			mapping_ = nullptr;
			const auto saved = errno;
			::close(fd_);
			fd_ = -1;
			errno = saved;
			throw_errno("map " + path.string());
		}
	}

	~MappedFile()
	{
		if (mapping_ != nullptr)
			::munmap(mapping_, size_);
		if (fd_ >= 0)
			::close(fd_);
	}

	MappedFile(const MappedFile &) = delete;
	MappedFile &operator=(const MappedFile &) = delete;

	[[nodiscard]] std::span<const std::byte> bytes() const noexcept
	{
		return {static_cast<const std::byte *>(mapping_), size_};
	}

private:
	int fd_ = -1;
	void *mapping_ = nullptr;
	std::size_t size_ = 0;
};

[[noreturn]] void throw_errno(const std::string &operation)
{
	throw std::runtime_error(operation + ": " + std::strerror(errno));
}

/*
 * Mirrors WaveformSettings::valid_decimation without coupling the capture
 * core to the settings library. Power-of-two divisors of the acquisition
 * rate; 32 bottoms out around 67 samples per 60 Hz cycle at 128 kSPS.
 */
bool valid_decimation(std::uint32_t decimation)
{
	return decimation == 1u || decimation == 2u || decimation == 4u ||
		decimation == 8u || decimation == 16u || decimation == 32u;
}

constexpr std::uint32_t trigger_source_bit(WaveformTriggerSource source)
{
	return 1u << static_cast<unsigned>(source);
}

std::uint32_t trigger_source_bit(std::uint16_t source)
{
	if (source < static_cast<std::uint16_t>(WaveformTriggerSource::manual_cli) ||
	    source > static_cast<std::uint16_t>(WaveformTriggerSource::pq_event))
		return 0u;
	return 1u << source;
}

bool uuid_is_zero(const MncwfUuid &uuid)
{
	return std::ranges::all_of(uuid,
		[](std::byte value) { return value == std::byte{0}; });
}

std::optional<std::uint64_t> session_id_from_filename(
	const std::filesystem::path &path)
{
	const auto filename = path.filename().string();
	constexpr std::string_view prefix = "waveform-";
	if (!filename.starts_with(prefix))
		return std::nullopt;
	const auto separator = filename.find('-', prefix.size());
	if (separator == std::string::npos || separator == prefix.size())
		return std::nullopt;
	std::uint64_t result = 0u;
	const auto first = filename.data() + prefix.size();
	const auto last = filename.data() + separator;
	const auto parsed = std::from_chars(first, last, result);
	if (parsed.ec != std::errc{} || parsed.ptr != last || result == 0u ||
	    result == std::numeric_limits<std::uint64_t>::max())
		return std::nullopt;
	return result;
}

std::uint32_t read_little_u32(std::span<const std::byte> bytes,
	std::size_t offset)
{
	if (offset > bytes.size() || bytes.size() - offset < 4u)
		throw std::out_of_range("read MNCWF version");
	std::uint32_t result = 0u;
	for (unsigned byte = 0; byte < 4u; ++byte)
		result |= static_cast<std::uint32_t>(
			std::to_integer<std::uint8_t>(bytes[offset + byte]))
			<< (byte * 8u);
	return result;
}

std::uint64_t translate_clock(std::uint64_t value,
	std::uint64_t source_anchor, std::uint64_t destination_anchor)
{
	if (value >= source_anchor) {
		const auto delta = value - source_anchor;
		if (delta > std::numeric_limits<std::uint64_t>::max() -
				destination_anchor)
			throw std::overflow_error("waveform clock translation overflow");
		return destination_anchor + delta;
	}
	const auto delta = source_anchor - value;
	if (delta > destination_anchor)
		throw std::overflow_error("waveform clock translation underflow");
	return destination_anchor - delta;
}

MncwfV4EventDescriptor manual_event_descriptor(std::uint64_t sequence,
	std::uint64_t tai_nanoseconds, std::uint64_t utc_nanoseconds,
	WaveformTriggerSource source, const WaveformCaptureContext &context)
{
	MncwfV4EventDescriptor event{};
	event.event_uuid = mncwf_random_uuid();
	event.taxonomy = MncwfEventTaxonomy::product_alarm;
	event.event_type = source == WaveformTriggerSource::manual_cli ? 0x100u
		: source == WaveformTriggerSource::manual_web ? 0x101u : 0x102u;
	event.lifecycle = MncwfEventLifecycle::complete;
	event.time_quality = context.time_quality;
	event.flags = mncwf_event_start_valid | mncwf_event_current_valid |
		mncwf_event_end_valid | mncwf_event_trigger_valid |
		mncwf_event_tai_valid | mncwf_event_utc_valid |
		mncwf_event_settings_snapshot_valid;
	event.phase_mask = mncwf_event_phase_system;
	event.quantity = MncwfQuantity::status;
	event.si_unit = MncwfSiUnit::dimensionless;
	event.trigger_source = static_cast<std::uint16_t>(source);
	event.start_sequence = sequence;
	event.current_sequence = sequence;
	event.end_sequence = sequence;
	event.trigger_sequence = sequence;
	event.start_tai_nanoseconds = tai_nanoseconds;
	event.current_tai_nanoseconds = tai_nanoseconds;
	event.end_tai_nanoseconds = tai_nanoseconds;
	event.trigger_tai_nanoseconds = tai_nanoseconds;
	event.start_utc_nanoseconds = utc_nanoseconds;
	event.current_utc_nanoseconds = utc_nanoseconds;
	event.end_utc_nanoseconds = utc_nanoseconds;
	event.trigger_utc_nanoseconds = utc_nanoseconds;
	event.update_count = 1u;
	event.taxonomy_name = "MSAP1 capture trigger";
	event.label = source == WaveformTriggerSource::manual_cli
		? "manual CLI capture"
		: source == WaveformTriggerSource::manual_web
			? "manual Web capture" : "power-quality capture";
	event.settings_snapshot_json = source == WaveformTriggerSource::manual_cli
		? R"({"trigger":"manual_cli"})"
		: source == WaveformTriggerSource::manual_web
			? R"({"trigger":"manual_web"})"
			: R"({"trigger":"power_quality"})";
	return event;
}

std::uint64_t tai_now_nanoseconds()
{
	timespec timestamp{};
	if (::clock_gettime(CLOCK_TAI, &timestamp) != 0)
		throw_errno("read CLOCK_TAI");
	return static_cast<std::uint64_t>(timestamp.tv_sec) * 1000000000ull +
		static_cast<std::uint64_t>(timestamp.tv_nsec);
}

std::uint64_t realtime_now_nanoseconds()
{
	timespec timestamp{};
	if (::clock_gettime(CLOCK_REALTIME, &timestamp) != 0)
		throw_errno("read CLOCK_REALTIME");
	return static_cast<std::uint64_t>(timestamp.tv_sec) * 1000000000ull +
		static_cast<std::uint64_t>(timestamp.tv_nsec);
}

std::string filename_timestamp(std::uint64_t realtime_nanoseconds)
{
	const auto seconds =
		static_cast<std::time_t>(realtime_nanoseconds / 1000000000ull);
	std::tm broken_down{};
	if (::gmtime_r(&seconds, &broken_down) == nullptr)
		throw std::runtime_error("convert waveform trigger time");
	std::ostringstream formatted;
	formatted << std::put_time(&broken_down, "%Y-%m-%d_%H-%M-%S-")
		  << std::setw(3) << std::setfill('0')
		  << (realtime_nanoseconds / 1000000ull) % 1000ull;
	return formatted.str();
}

/*
 * Version history: v3 adds the capture-file decimation divisor in the first
 * four reserved bytes. Sequences stay in the acquisition frame domain, so a
 * decimated file's frame_count is (last - first) / decimation + 1, not
 * last - first + 1.
 */
#pragma pack(push, 1)
struct WaveformFileHeaderV2 {
	std::array<char, 8> magic{};
	std::uint32_t version = 3;
	std::uint32_t header_bytes = 256;
	std::uint64_t session_id = 0;
	std::uint64_t first_sequence = 0;
	std::uint64_t last_sequence = 0;
	std::uint64_t trigger_sequence = 0;
	std::uint64_t trigger_tai_nanoseconds = 0;
	std::uint32_t sample_rate_hz = 0;
	std::uint32_t event_count = 0;
	std::uint64_t correlation_tai_nanoseconds = 0;
	std::uint64_t correlation_pl_tick = 0;
	std::uint64_t correlation_frame_sequence = 0;
	std::uint64_t correlation_uncertainty_nanoseconds = 0;
	std::uint32_t channel_count = waveform_persisted_channels;
	std::uint32_t frame_bytes =
		waveform_persisted_channels * sizeof(std::int32_t);
	std::uint32_t channel_descriptor_bytes =
		sizeof(WaveformChannelMetadata);
	std::uint32_t flags = 0;
	std::uint64_t channel_table_offset = 256;
	std::uint64_t event_table_offset = 0;
	std::uint64_t frame_data_offset = 0;
	std::uint64_t frame_count = 0;
	std::uint64_t trigger_realtime_nanoseconds = 0;
	std::uint32_t decimation = 1;
	std::array<std::byte, 100> reserved{};
};
#pragma pack(pop)

static_assert(sizeof(WaveformChannelMetadata) == 32);
static_assert(sizeof(WaveformFileHeaderV2) == 256);

} // namespace

WaveformSessionPage waveform_session_page(
	std::span<const WaveformSessionSummary> sessions,
	const WaveformSessionQuery &query)
{
	if (query.limit == 0u || query.limit > waveform_max_page_sessions)
		throw std::invalid_argument("waveform page limit must be 1..100");
	if (query.origin != WaveformOriginFilter::all &&
	    query.origin != WaveformOriginFilter::manual &&
	    query.origin != WaveformOriginFilter::power_quality)
		throw std::invalid_argument("unknown waveform origin filter");

	const auto manual_mask =
		trigger_source_bit(WaveformTriggerSource::manual_cli) |
		trigger_source_bit(WaveformTriggerSource::manual_web);
	const auto power_quality_mask =
		trigger_source_bit(WaveformTriggerSource::pq_event);
	const auto matches_origin = [&](const WaveformSessionSummary &session) {
		switch (query.origin) {
		case WaveformOriginFilter::all: return true;
		case WaveformOriginFilter::manual:
			return (session.trigger_source_mask & manual_mask) != 0u;
		case WaveformOriginFilter::power_quality:
			return (session.trigger_source_mask & power_quality_mask) != 0u;
		}
		return false;
	};

	WaveformSessionPage result{};
	result.origin = query.origin;
	result.limit = query.limit;
	std::vector<WaveformSessionSummary> candidates;
	candidates.reserve(sessions.size());
	for (const auto &session : sessions) {
		if (!matches_origin(session))
			continue;
		++result.total_sessions;
		switch (session.state) {
		case WaveformSessionState::capturing:
			++result.active_sessions;
			break;
		case WaveformSessionState::complete:
			++result.completed_sessions;
			break;
		case WaveformSessionState::incomplete:
			++result.incomplete_sessions;
			break;
		}
		if (query.before_session_id == 0u ||
		    session.id < query.before_session_id)
			candidates.push_back(session);
	}
	std::ranges::sort(candidates, std::greater{},
		&WaveformSessionSummary::id);
	const auto count = std::min<std::size_t>(query.limit, candidates.size());
	result.sessions.assign(candidates.begin(), candidates.begin() + count);
	if (candidates.size() > count && !result.sessions.empty())
		result.next_before_session_id = result.sessions.back().id;
	return result;
}

struct WaveformCapture::Event {
	WaveformEventIdentity identity{};
	bool stable_identity = false;
	MncwfV4EventDescriptor descriptor{};
};

struct WaveformCapture::Session {
	WaveformSessionSummary summary{};
	WaveformCaptureContext context{};
	std::vector<Event> events;
	std::vector<WaveformEventIdentity> active_events;
	MncwfUuid previous_capture_uuid{};
	MncwfUuid next_capture_uuid{};
	bool materialization_queued = false;
	bool capacity_sealed = false;
};

/*
 * Disk I/O is deliberately isolated from the acquisition thread. Writing a
 * multi-second capture can take longer than the kernel DMA transport slack;
 * blocking the epoll loop here would cause otherwise healthy WFM1 blocks to
 * be overwritten before userspace drains them.
 */
struct WaveformCapture::AsyncWriter {
	using Frame = std::array<std::int32_t, waveform_channels>;

	struct Job {
		WaveformSessionSummary summary{};
		WaveformCaptureContext context{};
		std::vector<MncwfV4EventDescriptor> events;
		std::vector<MncwfV4LineageEntry> lineage;
		WaveformCorrelation correlation{};
		std::uint64_t correlation_utc_nanoseconds = 0;
		std::filesystem::path output_directory;
		std::uint64_t source_frame_count = 0;
		std::uint64_t archive_limit_bytes =
			waveform_default_archive_limit_bytes;
		bool retention_only = false;
		std::vector<Frame> frames;
	};

	struct Result {
		std::uint64_t session_id = 0;
		bool success = false;
		std::string filename;
		std::string error;
		std::uint32_t format_version = 0;
		WaveformCompression compression = WaveformCompression::none;
		std::uint64_t stored_bytes = 0;
		std::uint64_t logical_sample_bytes = 0;
		std::uint64_t archive_stored_bytes = 0;
		std::vector<std::uint64_t> expired_session_ids;
	};

	AsyncWriter() : worker([this] { run(); }) {}

	~AsyncWriter()
	{
		{
			std::lock_guard lock(mutex);
			stopping = true;
		}
		ready.notify_one();
		if (worker.joinable())
			worker.join();
	}

	void enqueue(Job job)
	{
		{
			std::lock_guard lock(mutex);
			jobs.push_back(std::move(job));
		}
		ready.notify_one();
	}

	std::vector<Result> collect()
	{
		std::vector<Result> collected;
		std::lock_guard lock(mutex);
		collected.reserve(results.size());
		while (!results.empty()) {
			collected.push_back(std::move(results.front()));
			results.pop_front();
		}
		return collected;
	}

private:
	struct ArchiveCandidate {
		std::filesystem::path path;
		std::uint64_t bytes = 0;
		std::uint64_t created_utc_nanoseconds = 0;
		std::uint64_t session_id = 0;
	};

	static void log_expired_session(std::uint64_t session_id)
	{
		const std::array fields{
			mnc::logging::Field{"MNC_WAVEFORM_SESSION",
				std::to_string(session_id)}};
		(void)capture_log.write(mnc::logging::Priority::notice,
			"waveform capture expired by archive retention",
			"waveform_session_expired", fields);
	}

	static std::vector<Frame> decimate(const Job &job)
	{
		const auto divisor = std::max<std::uint32_t>(1u,
			job.summary.decimation);
		const auto output_count = (job.frames.size() + divisor - 1u) /
			divisor;
		std::vector<Frame> result;
		result.reserve(output_count);
		for (std::size_t first = 0; first < job.frames.size();
		     first += divisor) {
			const auto count = std::min<std::size_t>(divisor,
				job.frames.size() - first);
			std::array<std::int64_t, waveform_channels> sums{};
			for (std::size_t offset = 0; offset < count; ++offset)
				for (std::size_t channel = 0; channel < waveform_channels;
				     ++channel)
					sums[channel] += job.frames[first + offset][channel];
			Frame frame{};
			for (std::size_t channel = 0; channel < waveform_channels;
			     ++channel)
				frame[channel] = static_cast<std::int32_t>(
					sums[channel] / static_cast<std::int64_t>(count));
			result.push_back(frame);
		}
		return result;
	}

	static std::uint64_t enforce_retention(const Job &job,
		const std::filesystem::path &temporary, std::uint64_t new_bytes,
		std::vector<std::uint64_t> &expired)
	{
		std::vector<ArchiveCandidate> candidates;
		std::uint64_t physical_bytes = new_bytes;
		for (const auto &entry :
		     std::filesystem::directory_iterator(job.output_directory)) {
			if (!entry.is_regular_file() || entry.path() == temporary)
				continue;
			std::error_code size_error;
			const auto bytes = entry.file_size(size_error);
			if (size_error || bytes >
				std::numeric_limits<std::uint64_t>::max() - physical_bytes)
				throw std::runtime_error(
					"measure waveform archive for retention");
			physical_bytes += bytes;
			if (entry.path().extension() != ".mncwf")
				continue;
			const auto session_id = session_id_from_filename(entry.path());
			if (!session_id)
				continue;
			try {
				MappedFile mapped(entry.path());
				const MncwfV4Reader metadata(mapped.bytes(),
					MncwfValidationMode::metadata_only);
				candidates.push_back({entry.path(), bytes,
					metadata.capture_metadata().created_utc_nanoseconds,
					*session_id});
			} catch (const std::exception &) {
				// Unknown, incomplete, and malformed files count against the
				// ceiling but are never automatic deletion candidates.
			}
		}
		if (physical_bytes <= job.archive_limit_bytes)
			return physical_bytes;
		std::ranges::sort(candidates, [](const auto &left, const auto &right) {
			if (left.created_utc_nanoseconds != right.created_utc_nanoseconds)
				return left.created_utc_nanoseconds <
					right.created_utc_nanoseconds;
			return left.session_id < right.session_id;
		});
		std::vector<ArchiveCandidate> validated;
		std::uint64_t reclaimable = 0u;
		const auto required_reclaim = physical_bytes - job.archive_limit_bytes;
		auto next_candidate = std::size_t{0};
		for (; next_candidate < candidates.size() &&
		       reclaimable < required_reclaim; ++next_candidate) {
			const auto &candidate = candidates[next_candidate];
			try {
				MappedFile mapped(candidate.path);
				(void)MncwfV4Reader{mapped.bytes()};
				validated.push_back(candidate);
				reclaimable += candidate.bytes;
			} catch (const std::exception &) {
				// A malformed candidate remains untouched.
			}
		}
		if (reclaimable < required_reclaim)
			throw std::runtime_error(
				"waveform archive limit cannot be reclaimed safely");
		for (const auto &candidate : validated) {
			if (physical_bytes <= job.archive_limit_bytes)
				break;
			std::error_code remove_error;
			if (!std::filesystem::remove(candidate.path, remove_error) ||
			    remove_error)
				continue;
			physical_bytes -= candidate.bytes;
			expired.push_back(candidate.session_id);
			log_expired_session(candidate.session_id);
		}
		/* A concurrent/manual removal failure must not make a newer capture
		 * eligible without validating it first. Continue from the first
		 * candidate that was not part of the initial safety proof. */
		for (; physical_bytes > job.archive_limit_bytes &&
		       next_candidate < candidates.size(); ++next_candidate) {
			const auto &candidate = candidates[next_candidate];
			try {
				MappedFile mapped(candidate.path);
				(void)MncwfV4Reader{mapped.bytes()};
			} catch (const std::exception &) {
				continue;
			}
			std::error_code remove_error;
			if (!std::filesystem::remove(candidate.path, remove_error) ||
			    remove_error)
				continue;
			physical_bytes -= candidate.bytes;
			expired.push_back(candidate.session_id);
			log_expired_session(candidate.session_id);
		}
		if (physical_bytes > job.archive_limit_bytes)
			throw std::runtime_error(
				"waveform archive limit cannot be reclaimed safely");
		return physical_bytes;
	}

	static Result write(const Job &job)
	{
		Result result{};
		result.session_id = job.summary.id;
		if (job.retention_only) {
			try {
				result.archive_stored_bytes = enforce_retention(job, {}, 0u,
					result.expired_session_ids);
				result.success = true;
			} catch (const std::exception &failure) {
				result.error = failure.what();
			}
			return result;
		}

		std::ostringstream name;
		name << "waveform-" << job.summary.id << "-"
		     << filename_timestamp(
				job.summary.trigger_realtime_nanoseconds)
		     << ".mncwf";
		result.filename = name.str();
		const auto path = job.output_directory / result.filename;
		const auto temporary = path.string() + ".tmp";

		try {
			const auto persisted_frames = decimate(job);
			std::ofstream output(
				temporary, std::ios::binary | std::ios::trunc);
			if (!output)
				throw std::runtime_error(
					"create waveform file " + temporary);

			MncwfV4Document document{};
			document.capture_metadata = job.context.capture_metadata;
			document.timebase_segments.push_back({
				.first_frame = 0u,
				.frame_count = persisted_frames.size(),
				.first_sequence = job.summary.first_sequence,
				.sequence_step = job.summary.decimation,
				.acquisition_rate_numerator = job.summary.sample_rate_hz,
				.acquisition_rate_denominator = 1u,
				.persisted_rate_numerator = job.summary.sample_rate_hz,
				.persisted_rate_denominator = job.summary.decimation,
				.correlation_sequence = job.correlation.frame_sequence,
				.correlation_pl_tick = job.correlation.pl_tick,
				.correlation_tai_nanoseconds =
					job.correlation.tai_nanoseconds,
				.correlation_utc_nanoseconds =
					job.correlation_utc_nanoseconds,
				.uncertainty_nanoseconds =
					job.correlation.uncertainty_nanoseconds,
				.decimation_divisor = job.summary.decimation,
				.decimation_method = job.summary.decimation == 1u
					? MncwfDecimationMethod::none
					: MncwfDecimationMethod::boxcar_mean_toward_zero,
				.clock_source = job.context.clock_source,
				.time_quality = job.context.time_quality,
				.flags = job.context.time_flags,
				.utc_offset_seconds = job.context.utc_offset_seconds,
				.source_frame_count = job.source_frame_count,
			});
			document.channels = job.context.channels;
			document.events = job.events;
			document.lineage = job.lineage;
			document.sample_frame_count = persisted_frames.size();
			document.sample_frame_bytes = static_cast<std::uint32_t>(
				document.channels.size() * sizeof(std::int32_t));
			document.sample_data.reserve(persisted_frames.size() *
				document.sample_frame_bytes);
			for (const auto &frame : persisted_frames) {
				for (const auto &channel : document.channels) {
					if (channel.source_channel >= frame.size() ||
					    channel.storage_bits != 32u)
						throw std::runtime_error(
							"MNCWF channel geometry is unsupported");
					const auto value = std::bit_cast<std::uint32_t>(
						frame[channel.source_channel]);
					for (unsigned byte = 0; byte < 4u; ++byte)
						document.sample_data.push_back(
							static_cast<std::byte>(
								(value >> (byte * 8u)) & 0xffu));
				}
			}
			if (job.context.time_quality != MncwfTimeQuality::locked)
				document.quality_intervals.push_back({
					.first_frame = 0u,
					.frame_count = persisted_frames.size(),
					.first_sequence = job.summary.first_sequence,
					.last_sequence = job.summary.last_sequence,
					.channel_mask = 0u,
					.flags = mncwf_quality_timing_uncertain,
					.severity = 1u,
					.source = 1u,
					.detail_code = 0u,
				});
			const auto encoded = encode_mncwf_v5(document);
			output.write(reinterpret_cast<const char *>(encoded.data()),
				static_cast<std::streamsize>(encoded.size()));
			output.close();
			if (!output)
				throw std::runtime_error(
					"write waveform file " + temporary);
			MappedFile validation(temporary);
			const MncwfV4Reader persisted(validation.bytes());
			if (persisted.version() != mncwf_v5_version ||
			    !std::ranges::equal(persisted.sample_data(),
				    std::span<const std::byte>{document.sample_data}))
				throw std::runtime_error(
					"post-write MNCWF v5 validation mismatch");
			const auto zstd_chunks = std::ranges::count_if(
				persisted.sample_chunks(), [](const auto &chunk) {
					return chunk.codec == MncwfChunkCodec::zstd;
				});
			result.compression = zstd_chunks == 0u
				? WaveformCompression::raw_chunks
				: zstd_chunks == static_cast<std::ptrdiff_t>(
					persisted.sample_chunks().size())
					? WaveformCompression::zstd_chunks
					: WaveformCompression::mixed_raw_zstd_chunks;
			result.format_version = mncwf_v5_version;
			result.stored_bytes = encoded.size();
			result.logical_sample_bytes = document.sample_data.size();
			result.archive_stored_bytes = enforce_retention(job, temporary,
				result.stored_bytes, result.expired_session_ids);
			std::filesystem::rename(temporary, path);
			result.success = true;
		} catch (const std::exception &failure) {
			std::error_code ignored;
			std::filesystem::remove(temporary, ignored);
			result.success = false;
			result.error = failure.what();
		} catch (...) {
			std::error_code ignored;
			std::filesystem::remove(temporary, ignored);
			result.success = false;
			result.error = "unidentified write failure";
		}
		return result;
	}

	void run()
	{
		set_current_thread_nice(5, "waveform-writer");
		for (;;) {
			Job job;
			{
				std::unique_lock lock(mutex);
				ready.wait(lock,
					   [this] {
						   return stopping ||
							   !jobs.empty();
					   });
				if (stopping && jobs.empty())
					return;
				job = std::move(jobs.front());
				jobs.pop_front();
			}

			auto result = write(job);
			{
				std::lock_guard lock(mutex);
				results.push_back(std::move(result));
			}
		}
	}

	std::mutex mutex;
	std::condition_variable ready;
	std::deque<Job> jobs;
	std::deque<Result> results;
	std::thread worker;
	bool stopping = false;
};

WaveformCapture::WaveformCapture(std::string device_path,
				 std::filesystem::path output_directory,
				 WaveformCaptureContext context,
				 WaveformCaptureOptions options)
	: device_path_(std::move(device_path)),
	  output_directory_(std::move(output_directory)),
	  correlation_source_(std::move(options.correlation_source)),
	  archive_discovery_hook_(std::move(options.archive_discovery_hook)),
	  history_(options.history_capacity_frames),
	  writer_(std::make_unique<AsyncWriter>()),
	  archive_limit_bytes_(options.archive_limit_bytes)
{
	if (history_.empty())
		throw std::invalid_argument(
			"waveform history capacity must be non-zero");
	if (archive_limit_bytes_ == 0u)
		throw std::invalid_argument(
			"waveform archive limit must be non-zero");
	set_context(std::move(context));
}

void WaveformCapture::set_context(WaveformCaptureContext context)
{
	if (context.channels.empty() ||
	    context.channels.size() > waveform_persisted_channels)
		throw std::invalid_argument(
			"MNCWF v4 capture context has an invalid channel count");
	context_ = std::move(context);
}

void WaveformCapture::set_archive_limit_gib(std::uint32_t limit_gib)
{
	if (limit_gib < 1u || limit_gib > 16u)
		throw std::invalid_argument(
			"waveform archive limit must be 1..16 GiB");
	const auto requested = static_cast<std::uint64_t>(limit_gib) *
		1024ull * 1024ull * 1024ull;
	if (requested == archive_limit_bytes_)
		return;
	archive_limit_bytes_ = requested;
	if (fd_ >= 0 && archive_discovery_status().state ==
			WaveformArchiveDiscoveryState::complete) {
		AsyncWriter::Job retention{};
		retention.output_directory = output_directory_;
		retention.archive_limit_bytes = archive_limit_bytes_;
		retention.retention_only = true;
		writer_->enqueue(std::move(retention));
	}
}

void WaveformCapture::set_time_context(MncwfClockSource source,
	MncwfTimeQuality quality, std::uint16_t leap_flags) noexcept
{
	context_.clock_source = source;
	context_.time_quality = quality;
	context_.time_flags = static_cast<std::uint16_t>(
		(context_.time_flags & mncwf_time_utc_offset_known) |
		(leap_flags & (mncwf_time_positive_leap_pending |
			mncwf_time_negative_leap_pending)));
}

WaveformCapture::~WaveformCapture()
{
	stop();
	archive_discovery_worker_.request_stop();
	if (archive_discovery_worker_.joinable())
		archive_discovery_worker_.join();
	writer_.reset();
}

void WaveformCapture::start()
{
	if (fd_ >= 0)
		return;
	/*
	 * Closing and reopening the DMA device is an explicit continuity
	 * boundary. The PL sequence may legitimately restart or advance while
	 * Linux is rearming the channel, so old history must not establish the
	 * expected sequence for the new acquisition epoch.
	 */
	begin_stream_epoch();
	fd_ = ::open(device_path_.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd_ < 0)
		throw_errno("open " + device_path_);
	std::filesystem::create_directories(output_directory_);
	begin_persisted_session_discovery();
	transport_last_overrun_blocks_ = 0;
	if (const auto current = correlate())
		correlation_ = *current;
	update_transport_status();
}

void WaveformCapture::stop() noexcept
{
	if (fd_ >= 0)
		::close(fd_);
	fd_ = -1;
	try {
		collect_materialization_results();
	} catch (...) {
		// Shutdown must not fail because a background result cannot be read.
	}
	for (auto &session : sessions_) {
		if (session.summary.state == WaveformSessionState::capturing &&
		    !session.materialization_queued)
			session.summary.state = WaveformSessionState::incomplete;
	}
}

void WaveformCapture::begin_stream_epoch() noexcept
{
	for (auto &session : sessions_) {
		if (session.summary.state == WaveformSessionState::capturing &&
		    !session.materialization_queued) {
			session.summary.state = WaveformSessionState::incomplete;
			const std::array fields{
				mnc::logging::Field{"MNC_WAVEFORM_SESSION",
					std::to_string(session.summary.id)}};
			(void)capture_log.write(mnc::logging::Priority::warning,
				"waveform capture incomplete: acquisition "
				"stream epoch reset while capturing",
				"waveform_session_incomplete", fields);
		}
	}
	have_history_ = false;
	oldest_sequence_ = 0;
	latest_sequence_ = 0;
	sequence_gaps_ = 0;
	sample_rate_hz_ = 0;
	gaps_.clear();
	configuration_generation_.reset();
	correlation_ = {};
	correlation_utc_nanoseconds_ = 0u;
}

void WaveformCapture::begin_persisted_session_discovery()
{
	{
		std::scoped_lock lock(archive_discovery_mutex_);
		if (archive_discovery_.state !=
		    WaveformArchiveDiscoveryState::not_started)
			return;
	}

	std::vector<std::filesystem::path> files;
	archive_stored_bytes_ = 0u;
	for (const auto &entry :
	     std::filesystem::directory_iterator(output_directory_)) {
		if (!entry.is_regular_file())
			continue;
		std::error_code size_error;
		const auto stored = entry.file_size(size_error);
		if (!size_error && stored <=
			std::numeric_limits<std::uint64_t>::max() -
				archive_stored_bytes_)
			archive_stored_bytes_ += stored;
		if (entry.path().extension() != ".mncwf")
			continue;
		files.push_back(entry.path());

		/* New files encode the daemon session ID in their name. Reserve every
		 * such ID before the validation worker starts, including malformed
		 * files, so a capture created during discovery can never collide. */
		if (const auto id = session_id_from_filename(entry.path())) {
			next_session_id_ = std::max(next_session_id_, *id + 1u);
			continue;
		}

		/* Legacy files also used the generated name, but cheaply inspect the
		 * fixed header as a compatibility guard for renamed v1-v3 captures. */
		std::ifstream input(entry.path(), std::ios::binary);
		WaveformFileHeaderV2 header{};
		input.read(reinterpret_cast<char *>(&header), sizeof(header));
		if (input.gcount() >= 64 &&
		    header.magic == std::array<char, 8>{
			'M', 'N', 'C', 'W', 'F', '1', '\0', '\0'} &&
		    header.version >= 1u && header.version <= 3u &&
		    header.session_id > 0u &&
		    header.session_id < std::numeric_limits<std::uint64_t>::max())
			next_session_id_ = std::max(next_session_id_,
				header.session_id + 1u);
	}
	std::sort(files.begin(), files.end());

	{
		std::scoped_lock lock(archive_discovery_mutex_);
		archive_discovery_.state = files.empty()
			? WaveformArchiveDiscoveryState::complete
			: WaveformArchiveDiscoveryState::scanning;
		archive_discovery_.total_files = files.size();
		archive_discovery_result_ready_ = files.empty();
	}
	if (files.empty())
		return;

	archive_discovery_worker_ = std::jthread(
		[this, files = std::move(files)](std::stop_token stop) mutable {
			set_current_thread_nice(5, "waveform-archive-scanner");
			discover_persisted_sessions(stop, std::move(files));
		});
}

void WaveformCapture::discover_persisted_sessions(
	std::stop_token stop, std::vector<std::filesystem::path> files)
{
	auto read_session = [](const std::filesystem::path &path)
		-> std::optional<Session> {
		std::ifstream input(path, std::ios::binary);
		std::array<std::byte, 16> prefix{};
		input.read(reinterpret_cast<char *>(prefix.data()), prefix.size());
		if (input.gcount() != static_cast<std::streamsize>(prefix.size()) ||
		    !std::ranges::equal(std::span{prefix}.first(mncwf_magic.size()),
			mncwf_magic))
			return std::nullopt;
		const auto version = read_little_u32(prefix, 8u);
		if (version == mncwf_v4_version || version == mncwf_v5_version) {
			const auto session_id = session_id_from_filename(path);
			std::error_code error;
			const auto file_bytes = std::filesystem::file_size(path, error);
			if (!session_id || error ||
			    file_bytes < mncwf_v4_header_bytes ||
			    file_bytes > mncwf_v4_max_file_bytes)
				return std::nullopt;
			MappedFile mapped(path);
			const MncwfV4Reader reader(mapped.bytes(),
				MncwfValidationMode::metadata_only);
			const auto &segments = reader.timebase_segments();
			const auto &first = segments.front();
			const auto &last = segments.back();
			if (first.acquisition_rate_denominator == 0u ||
			    first.acquisition_rate_numerator %
				    first.acquisition_rate_denominator != 0u ||
			    last.source_frame_count == 0u)
				return std::nullopt;
			const auto sample_rate = first.acquisition_rate_numerator /
				first.acquisition_rate_denominator;
			if (sample_rate == 0u ||
			    sample_rate > std::numeric_limits<std::uint32_t>::max() ||
			    last.source_frame_count - 1u >
				std::numeric_limits<std::uint64_t>::max() -
					last.first_sequence ||
			    !std::ranges::all_of(segments,
				[&first](const auto &segment) {
					return segment.acquisition_rate_numerator ==
							first.acquisition_rate_numerator &&
					       segment.acquisition_rate_denominator ==
							first.acquisition_rate_denominator &&
					       segment.decimation_divisor ==
							first.decimation_divisor;
				}))
				return std::nullopt;

			Session session{};
			session.summary.id = *session_id;
			session.summary.capture_uuid =
				reader.capture_metadata().capture_uuid;
			session.summary.first_sequence = first.first_sequence;
			session.summary.last_sequence = last.first_sequence +
				last.source_frame_count - 1u;
			session.summary.trigger_sequence =
				session.summary.first_sequence;
			session.summary.trigger_tai_nanoseconds =
				reader.capture_metadata().created_tai_nanoseconds;
			session.summary.trigger_realtime_nanoseconds =
				reader.capture_metadata().created_utc_nanoseconds;
			const auto trigger = std::ranges::find_if(reader.events(),
				[](const auto &event) {
					return (event.flags &
						mncwf_event_trigger_valid) != 0u;
				});
			if (trigger != reader.events().end()) {
				session.summary.trigger_sequence =
					trigger->trigger_sequence;
				if ((trigger->flags & mncwf_event_tai_valid) != 0u)
					session.summary.trigger_tai_nanoseconds =
						trigger->trigger_tai_nanoseconds;
				if ((trigger->flags & mncwf_event_utc_valid) != 0u)
					session.summary.trigger_realtime_nanoseconds =
						trigger->trigger_utc_nanoseconds;
			}
			session.summary.sample_rate_hz =
				static_cast<std::uint32_t>(sample_rate);
			session.summary.event_count = static_cast<std::uint32_t>(
				reader.events().size());
			for (const auto &event : reader.events())
				session.summary.trigger_source_mask |=
					trigger_source_bit(event.trigger_source);
			session.summary.decimation = first.decimation_divisor;
			session.summary.master_session_id = *session_id;
			session.summary.state = WaveformSessionState::complete;
			session.summary.format_version = reader.version();
			session.summary.stored_bytes = file_bytes;
			session.summary.logical_sample_bytes =
				reader.sample_frame_count() * reader.sample_frame_bytes();
			if (reader.version() == mncwf_v5_version) {
				const auto zstd_chunks = std::ranges::count_if(
					reader.sample_chunks(), [](const auto &chunk) {
						return chunk.codec == MncwfChunkCodec::zstd;
					});
				session.summary.compression = zstd_chunks == 0u
					? WaveformCompression::raw_chunks
					: zstd_chunks == static_cast<std::ptrdiff_t>(
						reader.sample_chunks().size())
						? WaveformCompression::zstd_chunks
						: WaveformCompression::mixed_raw_zstd_chunks;
			}
			for (const auto &descriptor : reader.events())
				session.events.push_back({{},
					descriptor.trigger_source == static_cast<std::uint16_t>(
						WaveformTriggerSource::pq_event), descriptor});
			for (const auto &lineage : reader.lineage()) {
				if (lineage.relation ==
				    MncwfLineageRelation::previous_continuation)
					session.previous_capture_uuid =
						lineage.related_capture_uuid;
				else if (lineage.relation ==
					 MncwfLineageRelation::next_continuation)
					session.next_capture_uuid =
						lineage.related_capture_uuid;
			}
			const auto filename = path.filename().string();
			std::copy_n(filename.c_str(),
				std::min(filename.size(),
					session.summary.filename.size() - 1u),
				session.summary.filename.begin());
			return session;
		}

		if (version != 1u && version != 2u && version != 3u)
			return std::nullopt;
		input.clear();
		input.seekg(0, std::ios::beg);
		WaveformFileHeaderV2 header{};
		input.read(reinterpret_cast<char *>(&header), sizeof(header));
		const auto bytes_read = static_cast<std::size_t>(input.gcount());
		if (input.gcount() < 64 ||
		    header.magic != std::array<char, 8>{
			'M', 'N', 'C', 'W', 'F', '1', '\0', '\0'} ||
		    header.version != version || header.session_id == 0u ||
		    header.session_id == std::numeric_limits<std::uint64_t>::max())
			return std::nullopt;

		const std::uint32_t decimation =
			header.version >= 3u ? header.decimation : 1u;
		if (!valid_decimation(decimation))
			return std::nullopt;

		Session session{};
		session.summary.id = header.session_id;
		session.summary.first_sequence = header.first_sequence;
		session.summary.last_sequence = header.last_sequence;
		session.summary.trigger_sequence = header.trigger_sequence;
		session.summary.trigger_tai_nanoseconds =
			header.trigger_tai_nanoseconds;
		session.summary.sample_rate_hz = header.sample_rate_hz;
		session.summary.event_count = header.event_count;
		session.summary.decimation = decimation;
		session.summary.master_session_id = header.session_id;
		session.summary.state = WaveformSessionState::complete;
		session.summary.format_version = version;

		std::uint64_t expected_file_bytes = 0;
		if (header.version >= 2u && bytes_read == sizeof(header) &&
		    header.header_bytes == sizeof(header) &&
		    header.channel_count > 0u &&
		    header.channel_count <= waveform_channels &&
		    header.channel_descriptor_bytes ==
			    sizeof(WaveformChannelMetadata) &&
		    header.frame_bytes ==
			    header.channel_count * sizeof(std::int32_t) &&
		    header.channel_table_offset == sizeof(header) &&
		    header.event_table_offset ==
			    header.channel_table_offset +
				    header.channel_count *
					    header.channel_descriptor_bytes &&
		    header.frame_data_offset ==
			    header.event_table_offset +
				    static_cast<std::uint64_t>(header.event_count) * 24u &&
		    header.last_sequence >= header.first_sequence &&
		    (header.last_sequence - header.first_sequence) % decimation == 0u &&
		    header.frame_count ==
			    (header.last_sequence - header.first_sequence) /
				    decimation + 1u) {
			expected_file_bytes = header.frame_data_offset +
				header.frame_count * header.frame_bytes;
			session.summary.trigger_realtime_nanoseconds =
				header.trigger_realtime_nanoseconds;
		} else if (header.version == 1u &&
			   header.header_bytes == 128u &&
			   header.last_sequence >= header.first_sequence) {
			const auto frame_count =
				header.last_sequence - header.first_sequence + 1u;
			expected_file_bytes = 128u +
				static_cast<std::uint64_t>(header.event_count) * 24u +
				frame_count * waveform_frame_bytes;
			std::error_code error;
			const auto modified =
				std::filesystem::last_write_time(path, error);
			if (!error) {
				const auto system_time =
					std::chrono::system_clock::now() +
					(modified -
					 std::filesystem::file_time_type::clock::now());
				session.summary.trigger_realtime_nanoseconds =
					static_cast<std::uint64_t>(
						std::chrono::duration_cast<
							std::chrono::nanoseconds>(
							system_time.time_since_epoch())
							.count());
			}
		} else {
			return std::nullopt;
		}
		std::error_code error;
		const auto file_bytes = std::filesystem::file_size(path, error);
		if (error || file_bytes != expected_file_bytes)
			return std::nullopt;
		session.summary.stored_bytes = file_bytes;
		session.summary.logical_sample_bytes = expected_file_bytes >=
			header.frame_data_offset
			? expected_file_bytes - header.frame_data_offset : 0u;

		const auto filename = path.filename().string();
		std::copy_n(filename.c_str(),
			std::min(filename.size(),
				session.summary.filename.size() - 1u),
			session.summary.filename.begin());
		return session;
	};

	try {
		std::vector<Session> restored;
		for (const auto &path : files) {
			if (stop.stop_requested())
				break;
			if (archive_discovery_hook_)
				archive_discovery_hook_(stop, path);
			if (stop.stop_requested())
				break;

			bool accepted = false;
			try {
				if (auto session = read_session(path)) {
					restored.push_back(std::move(*session));
					accepted = true;
				}
			} catch (const std::exception &) {
				/* Malformed, unsupported, or unreadable files stay hidden. */
			}
			std::scoped_lock lock(archive_discovery_mutex_);
			++archive_discovery_.scanned_files;
			if (!accepted)
				++archive_discovery_.rejected_files;
		}

		std::scoped_lock lock(archive_discovery_mutex_);
		if (stop.stop_requested()) {
			archive_discovery_.state =
				WaveformArchiveDiscoveryState::cancelled;
			return;
		}
		discovered_sessions_ = std::move(restored);
		archive_discovery_result_ready_ = true;
	} catch (const std::exception &error) {
		std::scoped_lock lock(archive_discovery_mutex_);
		archive_discovery_error_ = error.what();
		archive_discovery_.state = WaveformArchiveDiscoveryState::failed;
	} catch (...) {
		std::scoped_lock lock(archive_discovery_mutex_);
		archive_discovery_error_ = "unknown archive discovery failure";
		archive_discovery_.state = WaveformArchiveDiscoveryState::failed;
	}
}

void WaveformCapture::collect_discovery_results()
{
	std::vector<Session> restored;
	std::string failure;
	bool completed = false;
	{
		std::scoped_lock lock(archive_discovery_mutex_);
		if (!archive_discovery_error_.empty() &&
		    !archive_discovery_error_reported_) {
			failure = archive_discovery_error_;
			archive_discovery_error_reported_ = true;
		}
		if (archive_discovery_result_ready_ &&
		    !archive_discovery_result_collected_) {
			restored = std::move(discovered_sessions_);
			archive_discovery_result_collected_ = true;
			completed = true;
		}
	}
	if (!failure.empty())
		(void)capture_log.write(mnc::logging::Priority::warning,
			"waveform archive discovery failed: " + failure,
			"waveform_archive_discovery_failed");
	if (!completed)
		return;

	std::uint64_t merge_rejections = 0u;
	for (auto &session : restored) {
		const bool duplicate = std::ranges::any_of(sessions_,
			[&session](const Session &candidate) {
				return candidate.summary.id == session.summary.id;
			});
		if (duplicate) {
			++merge_rejections;
			continue;
		}
		sessions_.push_back(std::move(session));
	}
	if (merge_rejections != 0u) {
		std::scoped_lock lock(archive_discovery_mutex_);
		archive_discovery_.rejected_files += merge_rejections;
	}
	std::sort(sessions_.begin(), sessions_.end(),
		[](const Session &left, const Session &right) {
			return left.summary.id < right.summary.id;
		});
	rebuild_session_lineage();
	if (!startup_retention_queued_) {
		AsyncWriter::Job retention{};
		retention.output_directory = output_directory_;
		retention.archive_limit_bytes = archive_limit_bytes_;
		retention.retention_only = true;
		writer_->enqueue(std::move(retention));
		startup_retention_queued_ = true;
	}
	{
		std::scoped_lock lock(archive_discovery_mutex_);
		archive_discovery_.state = WaveformArchiveDiscoveryState::complete;
	}
}

void WaveformCapture::rebuild_session_lineage()
{
	/* MNCWF persists UUID lineage rather than daemon-local session IDs. Resolve
	 * it in one ascending pass after every validated file has been merged. */
	rebuild_indexes();
	for (auto &session : sessions_) {
		session.summary.master_session_id = session.summary.id;
		session.summary.continuation_of_session_id = 0u;
		if (uuid_is_zero(session.previous_capture_uuid))
			continue;
		const auto indexed = capture_index_.find(
			mncwf_uuid_string(session.previous_capture_uuid));
		if (indexed == capture_index_.end())
			continue;
		const auto &predecessor = sessions_[indexed->second];
		if (predecessor.summary.id >= session.summary.id ||
		    predecessor.summary.last_sequence ==
			std::numeric_limits<std::uint64_t>::max() ||
		    predecessor.summary.last_sequence + 1u !=
			session.summary.first_sequence ||
		    predecessor.next_capture_uuid != session.summary.capture_uuid)
			continue;
		session.summary.continuation_of_session_id =
			predecessor.summary.id;
	}
	for (std::size_t index = 0; index < sessions_.size(); ++index) {
		auto &session = sessions_[index];
		const auto predecessor = session_index_.find(
			session.summary.continuation_of_session_id);
		if (predecessor != session_index_.end() &&
		    predecessor->second < index)
			session.summary.master_session_id =
				sessions_[predecessor->second].summary.master_session_id;
	}
	rebuild_event_capture_index();
}

void WaveformCapture::rebuild_indexes()
{
	session_index_.clear();
	capture_index_.clear();
	session_index_.reserve(sessions_.size());
	capture_index_.reserve(sessions_.size());
	for (std::size_t index = 0; index < sessions_.size(); ++index) {
		session_index_.insert_or_assign(sessions_[index].summary.id, index);
		if (!uuid_is_zero(sessions_[index].summary.capture_uuid))
			capture_index_.insert_or_assign(
				mncwf_uuid_string(sessions_[index].summary.capture_uuid), index);
	}
}

void WaveformCapture::rebuild_event_capture_index()
{
	event_capture_index_.clear();
	for (const auto &session : sessions_) {
		for (const auto &event : session.events) {
			if (event.descriptor.trigger_source !=
			    static_cast<std::uint16_t>(WaveformTriggerSource::pq_event) ||
			    uuid_is_zero(event.descriptor.event_uuid))
				continue;
			auto &association = event_capture_index_[
				mncwf_uuid_string(event.descriptor.event_uuid)];
			if (association.onset_session_id == 0u)
				association.onset_session_id = session.summary.id;
			else if (association.onset_session_id != session.summary.id &&
				 association.recovery_session_id == 0u)
				association.recovery_session_id = session.summary.id;
			association.terminal = association.terminal ||
				event.descriptor.lifecycle == MncwfEventLifecycle::end ||
				event.descriptor.lifecycle == MncwfEventLifecycle::abort ||
				event.descriptor.lifecycle == MncwfEventLifecycle::complete;
		}
	}
}

WaveformArchiveDiscoveryStatus
WaveformCapture::archive_discovery_status() const
{
	std::scoped_lock lock(archive_discovery_mutex_);
	return archive_discovery_;
}

std::optional<WaveformCorrelation> WaveformCapture::correlate() const noexcept
{
	if (fd_ < 0 || !correlation_source_)
		return std::nullopt;
	try {
		return correlation_source_();
	} catch (...) {
		return std::nullopt;
	}
}

void WaveformCapture::update_transport_status() noexcept
{
	if (fd_ < 0)
		return;
	WaveformTransportStatusIoctl status{};
	if (::ioctl(fd_, waveform_transport_status_ioctl, &status) != 0)
		return;
	transport_ring_blocks_ = status.ring_blocks;
	if (status.overrun_blocks >= transport_last_overrun_blocks_) {
		transport_overrun_blocks_ +=
			status.overrun_blocks - transport_last_overrun_blocks_;
	}
	transport_last_overrun_blocks_ = status.overrun_blocks;
}

void WaveformCapture::read_available()
{
	collect_materialization_results();
	update_transport_status();

	std::array<WaveformBlock, 2> blocks{};
	for (;;) {
		const auto size = ::read(fd_, blocks.data(), sizeof(blocks));
		if (size < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK ||
			    errno == EINTR)
				break;
			throw_errno("read " + device_path_);
		}
		if (size == 0)
			break;
		bytes_ += static_cast<std::uint64_t>(size);
		if (size % static_cast<ssize_t>(sizeof(WaveformBlock)) != 0) {
			++invalid_blocks_;
			continue;
		}
		const auto count =
			static_cast<std::size_t>(size) / sizeof(WaveformBlock);
		for (std::size_t index = 0; index < count; ++index)
			accept_block(blocks[index]);
	}
	update_transport_status();
	finish_sessions();
	collect_materialization_results();
}

void WaveformCapture::accept_block(const WaveformBlock &block)
{
	const auto &header = block.header;
	if (header.magic != waveform_block_magic ||
	    header.version != waveform_block_version ||
	    header.block_bytes != waveform_block_bytes ||
	    header.frame_count != waveform_frames_per_block ||
	    header.frame_bytes != waveform_frame_bytes) {
		++invalid_blocks_;
		return;
	}

	/*
	 * A generation change is a second defensive epoch boundary. Normal
	 * source/configuration transactions close and reopen the DMA device, but
	 * this also prevents an unexpected in-place generation change from being
	 * reported as lost waveform frames or joining two sources in one history.
	 */
	if (configuration_generation_ &&
	    *configuration_generation_ != header.configuration_generation) {
		begin_stream_epoch();
		if (const auto current = correlate())
			correlation_ = *current;
	}
	configuration_generation_ = header.configuration_generation;

	const auto first = header.first_sequence();
	if (have_history_) {
		const auto expected = latest_sequence_ + 1u;
		if (first < expected) {
			/*
			 * Never rewind the history on a stale/out-of-order DMA
			 * period. Doing so makes a later session appear contiguous
			 * while silently replacing newer frames.
			 */
			++invalid_blocks_;
			return;
		}
		if (first > expected) {
			sequence_gaps_ += first - expected;
			gaps_.push_back({expected, first - 1u});
		}
	}
	for (std::size_t index = 0; index < waveform_frames_per_block; ++index) {
		const auto sequence = first + index;
		history_[sequence % history_.size()] = block.frames[index];
	}
	latest_sequence_ = first + waveform_frames_per_block - 1u;
	if (!have_history_) {
		oldest_sequence_ = first;
		have_history_ = true;
	} else if (latest_sequence_ - oldest_sequence_ + 1u >
		   history_.size()) {
		oldest_sequence_ = latest_sequence_ - history_.size() + 1u;
	}
	gaps_.erase(std::remove_if(gaps_.begin(), gaps_.end(),
			    [this](const GapRange &gap) {
				    return gap.last < oldest_sequence_;
			    }),
		    gaps_.end());
	sample_rate_hz_ = header.measured_sample_rate_hz;
	/*
	 * The PL reports its own cumulative drop counter in every block
	 * header; an increase means the elasticity FIFO overflowed because
	 * the DMA stopped draining — loss upstream of everything the
	 * transport counters can see.
	 */
	if (header.dropped_frames > pl_dropped_frames_) {
		const std::array fields{
			mnc::logging::Field{"MNC_PL_DROPPED_FRAMES",
				std::to_string(header.dropped_frames)}};
		(void)capture_log.write(mnc::logging::Priority::warning,
			"PL waveform branch dropped frames upstream of the DMA",
			"waveform_pl_drops", fields);
	}
	pl_dropped_frames_ = header.dropped_frames;
	++blocks_;
	frames_ += waveform_frames_per_block;
}

WaveformSessionSummary WaveformCapture::trigger(
	std::uint32_t pretrigger_ms, std::uint32_t posttrigger_ms,
	std::uint32_t decimation, WaveformTriggerSource source)
{
	collect_materialization_results();
	if (!have_history_ || sample_rate_hz_ == 0)
		throw std::runtime_error("waveform history is not ready");
	if (pretrigger_ms > 120000u || posttrigger_ms > 120000u)
		throw std::invalid_argument("waveform duration exceeds 120 seconds");
	if (!valid_decimation(decimation))
		throw std::invalid_argument(
			"waveform decimation must be 1, 2, 4, 8, 16, or 32");

	const auto frames_for = [this](std::uint32_t milliseconds) {
		return (static_cast<std::uint64_t>(sample_rate_hz_) * milliseconds +
			999u) /
			1000u;
	};
	const auto budget = max_capture_frames();
	const auto requested =
		frames_for(pretrigger_ms) + frames_for(posttrigger_ms) + 1u;
	if (requested > budget) {
		const auto maximum_ms = budget * 1000u / sample_rate_hz_;
		std::ostringstream message;
		message << "waveform window of "
			<< (pretrigger_ms + posttrigger_ms)
			<< " ms does not fit the history buffer at "
			<< sample_rate_hz_ << " frame/s; pre+post may total "
			<< maximum_ms << " ms";
		throw std::invalid_argument(message.str());
	}
	const auto anchor = latest_sequence_;
	const auto requested_first =
		anchor > frames_for(pretrigger_ms)
			? anchor - frames_for(pretrigger_ms)
			: 0u;
	const auto requested_last = anchor + frames_for(posttrigger_ms);
	/*
	 * Refresh the PL-tick/CLOCK_TAI mapping at every trigger. A startup-only
	 * correlation would accumulate oscillator drift during long uptimes.
	 */
	if (const auto current = correlate())
		correlation_ = *current;
	const auto now_tai = tai_now_nanoseconds();
	const auto now_realtime = realtime_now_nanoseconds();
	if (correlation_.tai_nanoseconds != 0u)
		correlation_utc_nanoseconds_ = translate_clock(
			correlation_.tai_nanoseconds, now_tai, now_realtime);

	auto active = std::find_if(sessions_.begin(), sessions_.end(),
		[](const Session &session) {
			return session.summary.state ==
				       WaveformSessionState::capturing &&
				!session.materialization_queued &&
				!session.capacity_sealed;
		});
	if (active == sessions_.end() ||
	    requested_first > active->summary.last_sequence + 1u) {
		Session session{};
		session.summary.id = next_session_id_++;
		session.summary.trigger_sequence = anchor;
		session.summary.first_sequence = requested_first;
		session.summary.last_sequence = requested_last;
		session.summary.trigger_tai_nanoseconds = now_tai;
		session.summary.trigger_realtime_nanoseconds = now_realtime;
		session.summary.sample_rate_hz = sample_rate_hz_;
		session.summary.decimation = decimation;
		session.summary.format_version = mncwf_v5_version;
		/* The background writer reports the effective raw/Zstd mix. */
		session.summary.compression = WaveformCompression::none;
		session.summary.state = WaveformSessionState::capturing;
		session.summary.master_session_id = session.summary.id;
		session.summary.capture_uuid = mncwf_random_uuid();
		session.context = context_;
		session.context.capture_metadata.capture_uuid =
			session.summary.capture_uuid;
		session.context.capture_metadata.created_tai_nanoseconds = now_tai;
		session.context.capture_metadata.created_utc_nanoseconds = now_realtime;
		sessions_.push_back(std::move(session));
		rebuild_indexes();
		active = std::prev(sessions_.end());
	} else {
		active->summary.first_sequence =
			std::min(active->summary.first_sequence, requested_first);
		active->summary.last_sequence =
			std::max(active->summary.last_sequence, requested_last);
		/*
		 * A window cannot mix sample rates, so an overlapping trigger
		 * extends the active session at its original decimation.
		 */
		if (active->summary.decimation != decimation) {
			const std::array fields{
				mnc::logging::Field{"MNC_WAVEFORM_SESSION",
					std::to_string(active->summary.id)},
				mnc::logging::Field{"MNC_DECIMATION",
					std::to_string(
						active->summary.decimation)}};
			(void)capture_log.write(mnc::logging::Priority::notice,
				"overlapping trigger keeps the active "
				"session's decimation",
				"waveform_decimation_kept", fields);
		}
	}
	active->events.push_back({{}, false,
		manual_event_descriptor(anchor, now_tai, now_realtime, source,
			active->context)});
	active->summary.event_count =
		static_cast<std::uint32_t>(active->events.size());
	active->summary.trigger_source_mask |= trigger_source_bit(source);
	active->summary.logical_sample_bytes =
		((active->summary.last_sequence - active->summary.first_sequence + 1u +
		  active->summary.decimation - 1u) / active->summary.decimation) *
		active->context.channels.size() * sizeof(std::int32_t);
	return active->summary;
}

WaveformSessionSummary WaveformCapture::track_power_quality_event(
	WaveformEventIdentity event_id, WaveformEventLifecycle lifecycle,
	std::uint64_t trigger_sequence, std::uint64_t current_sequence,
	std::uint32_t pretrigger_ms, std::uint32_t posttrigger_ms,
	std::uint32_t decimation, MncwfV4EventDescriptor descriptor)
{
	collect_materialization_results();
	if (event_id.session == 0u || event_id.counter == 0u)
		throw std::invalid_argument("power-quality event ID is zero");
	if (!have_history_ || sample_rate_hz_ == 0)
		throw std::runtime_error("waveform history is not ready");
	if (trigger_sequence > current_sequence)
		throw std::invalid_argument("event trigger is after its current sample");
	if (pretrigger_ms > 120000u || posttrigger_ms > 120000u)
		throw std::invalid_argument("waveform duration exceeds 120 seconds");
	if (!valid_decimation(decimation))
		throw std::invalid_argument(
			"waveform decimation must be 1, 2, 4, 8, 16, or 32");
	if (uuid_is_zero(descriptor.event_uuid))
		throw std::invalid_argument("power-quality event UUID is zero");
	const auto frames_for = [this](std::uint32_t milliseconds) {
		return (static_cast<std::uint64_t>(sample_rate_hz_) * milliseconds +
			999u) / 1000u;
	};
	const auto pretrigger_frames = frames_for(pretrigger_ms);
	const auto posttrigger_frames = frames_for(posttrigger_ms);
	const auto budget = max_capture_frames();
	if (pretrigger_frames + posttrigger_frames + 1u > budget)
		throw std::invalid_argument(
			"one event waveform policy exceeds the history budget");

	if (const auto current = correlate())
		correlation_ = *current;
	const auto now_tai = tai_now_nanoseconds();
	const auto now_realtime = realtime_now_nanoseconds();
	if (correlation_.tai_nanoseconds != 0u)
		correlation_utc_nanoseconds_ = translate_clock(
			correlation_.tai_nanoseconds, now_tai, now_realtime);

	const auto event_key = mncwf_uuid_string(descriptor.event_uuid);
	const auto session_for = [this](std::uint64_t id) -> Session * {
		const auto found = session_index_.find(id);
		return found == session_index_.end() ? nullptr
			: &sessions_[found->second];
	};
	const auto response = [](const Session &session, bool created) {
		auto result = session.summary;
		result.association_created = created;
		return result;
	};
	const auto update_marker = [&](Session &session,
		const MncwfV4EventDescriptor &updated) {
		if (session.summary.state != WaveformSessionState::capturing ||
		    session.materialization_queued)
			return;
		const auto marker = std::ranges::find_if(session.events,
			[&updated](const Event &event) {
				return event.descriptor.event_uuid == updated.event_uuid;
			});
		if (marker == session.events.end())
			session.events.push_back({event_id, true, updated});
		else
			marker->descriptor = updated;
		session.summary.event_count = static_cast<std::uint32_t>(
			session.events.size());
	};
	const auto bounded_window = [&](std::uint64_t anchor) {
		if (anchor > std::numeric_limits<std::uint64_t>::max() -
				posttrigger_frames)
			throw std::invalid_argument("event post-trigger range overflows");
		auto first = anchor > pretrigger_frames
			? anchor - pretrigger_frames : 0u;
		auto last = anchor + posttrigger_frames;
		if (last < oldest_sequence_)
			throw std::runtime_error(
				"event waveform window is no longer retained");
		first = std::max(first, oldest_sequence_);
		if (last - first + 1u > budget)
			first = last - budget + 1u;
		return std::pair{first, last};
	};
	const auto new_session = [&](std::uint64_t first, std::uint64_t last) ->
		Session & {
		Session session{};
		session.summary.id = next_session_id_++;
		session.summary.trigger_sequence = lifecycle ==
			WaveformEventLifecycle::start ? trigger_sequence : current_sequence;
		session.summary.first_sequence = first;
		session.summary.last_sequence = last;
		session.summary.trigger_tai_nanoseconds = now_tai;
		session.summary.trigger_realtime_nanoseconds = now_realtime;
		session.summary.sample_rate_hz = sample_rate_hz_;
		session.summary.decimation = decimation;
		session.summary.format_version = mncwf_v5_version;
		/* The background writer reports the effective raw/Zstd mix. */
		session.summary.compression = WaveformCompression::none;
		session.summary.state = WaveformSessionState::capturing;
		session.summary.master_session_id = session.summary.id;
		session.summary.capture_uuid = mncwf_random_uuid();
		session.summary.trigger_source_mask =
			trigger_source_bit(WaveformTriggerSource::pq_event);
		session.context = context_;
		session.context.capture_metadata.capture_uuid =
			session.summary.capture_uuid;
		session.context.capture_metadata.created_tai_nanoseconds = now_tai;
		session.context.capture_metadata.created_utc_nanoseconds = now_realtime;
		session.events.push_back({event_id, true, descriptor});
		session.summary.event_count = 1u;
		session.summary.logical_sample_bytes =
			((last - first + 1u + decimation - 1u) / decimation) *
			session.context.channels.size() * sizeof(std::int32_t);
		sessions_.push_back(std::move(session));
		rebuild_indexes();
		return sessions_.back();
	};

	auto association = event_capture_index_.find(event_key);
	if (association == event_capture_index_.end()) {
		const auto anchor = lifecycle == WaveformEventLifecycle::start
			? trigger_sequence : current_sequence;
		const auto [first, last] = bounded_window(anchor);
		auto &created = new_session(first, last);
		EventCaptureState state{};
		if (lifecycle == WaveformEventLifecycle::start)
			state.onset_session_id = created.summary.id;
		else
			state.recovery_session_id = created.summary.id;
		state.terminal = lifecycle == WaveformEventLifecycle::end ||
			lifecycle == WaveformEventLifecycle::abort;
		event_capture_index_.insert_or_assign(event_key, state);
		return response(created, true);
	}

	auto &state = association->second;
	Session *onset = session_for(state.onset_session_id);
	Session *recovery = session_for(state.recovery_session_id);
	Session *latest = recovery != nullptr ? recovery : onset;
	if (latest == nullptr) {
		event_capture_index_.erase(association);
		const auto [first, last] = bounded_window(
			lifecycle == WaveformEventLifecycle::start
				? trigger_sequence : current_sequence);
		auto &created = new_session(first, last);
		EventCaptureState replacement{};
		if (lifecycle == WaveformEventLifecycle::start)
			replacement.onset_session_id = created.summary.id;
		else
			replacement.recovery_session_id = created.summary.id;
		replacement.terminal = lifecycle == WaveformEventLifecycle::end ||
			lifecycle == WaveformEventLifecycle::abort;
		event_capture_index_.insert_or_assign(event_key, replacement);
		return response(created, true);
	}

	if (lifecycle == WaveformEventLifecycle::start ||
	    lifecycle == WaveformEventLifecycle::update || state.terminal ||
	    recovery != nullptr) {
		update_marker(*latest, descriptor);
		return response(*latest, false);
	}

	const auto [recovery_first, recovery_last] =
		bounded_window(current_sequence);
	if (onset != nullptr &&
	    onset->summary.state == WaveformSessionState::capturing &&
	    !onset->materialization_queued &&
	    recovery_first <= onset->summary.last_sequence + 1u &&
	    onset->summary.first_sequence <= recovery_last + 1u) {
		const auto merged_first = std::min(onset->summary.first_sequence,
			recovery_first);
		const auto merged_last = std::max(onset->summary.last_sequence,
			recovery_last);
		if (merged_last - merged_first + 1u <= budget) {
			onset->summary.first_sequence = merged_first;
			onset->summary.last_sequence = merged_last;
			onset->summary.logical_sample_bytes =
				((merged_last - merged_first + 1u + decimation - 1u) /
				 decimation) * onset->context.channels.size() *
				sizeof(std::int32_t);
			update_marker(*onset, descriptor);
			state.terminal = true;
			return response(*onset, false);
		}
	}

	auto &created = new_session(recovery_first, recovery_last);
	state.recovery_session_id = created.summary.id;
	state.terminal = true;
	return response(created, true);
}

void WaveformCapture::erase(std::uint64_t session_id)
{
	collect_discovery_results();
	collect_materialization_results();
	const auto session = std::find_if(
		sessions_.begin(), sessions_.end(),
		[session_id](const Session &candidate) {
			return candidate.summary.id == session_id;
		});
	if (session == sessions_.end())
		throw std::invalid_argument("waveform session does not exist");
	if (session->summary.state == WaveformSessionState::capturing ||
	    session->materialization_queued)
		throw std::runtime_error("waveform session is still active");

	const std::string filename = session->summary.filename.data();
	if (!filename.empty()) {
		const std::filesystem::path relative(filename);
		if (relative != relative.filename())
			throw std::runtime_error("waveform filename is invalid");
		std::error_code size_error;
		const auto stored = std::filesystem::file_size(
			output_directory_ / relative, size_error);
		std::error_code error;
		const auto removed = std::filesystem::remove(
			output_directory_ / relative, error);
		if (error)
			throw std::runtime_error(
				"delete waveform file " + filename + ": " +
				error.message());
		if (removed && !size_error)
			archive_stored_bytes_ = stored < archive_stored_bytes_
				? archive_stored_bytes_ - stored : 0u;
	}
	sessions_.erase(session);
	rebuild_indexes();
	rebuild_event_capture_index();
}

bool WaveformCapture::intersects_gap(std::uint64_t first,
				     std::uint64_t last) const
{
	return std::any_of(gaps_.begin(), gaps_.end(),
		[first, last](const GapRange &gap) {
			return gap.first <= last && gap.last >= first;
		});
}

std::uint64_t WaveformCapture::max_capture_frames() const noexcept
{
	/*
	 * The history ring is sized in frames, so its span in seconds shrinks
	 * as the sample rate rises; a fixed millisecond limit silently stops
	 * fitting when the rate changes. Frames also keep arriving between
	 * the post-trigger window closing and the session being materialized
	 * (up to one 250 ms poll plus queueing), so hold back two seconds of
	 * frames as margin against the oldest frames being evicted mid-copy.
	 */
	const auto margin = static_cast<std::uint64_t>(sample_rate_hz_) * 2u;
	if (margin >= history_.size())
		return 0;
	return history_.size() - margin;
}

void WaveformCapture::finish_sessions()
{
	for (auto &session : sessions_) {
		if (session.summary.state != WaveformSessionState::capturing ||
		    session.materialization_queued ||
		    !session.active_events.empty() ||
		    latest_sequence_ < session.summary.last_sequence)
			continue;
		const bool evicted =
			session.summary.first_sequence < oldest_sequence_;
		if (evicted || intersects_gap(session.summary.first_sequence,
					      session.summary.last_sequence)) {
			session.summary.state = WaveformSessionState::incomplete;
			const std::array fields{
				mnc::logging::Field{"MNC_WAVEFORM_SESSION",
					std::to_string(session.summary.id)},
				mnc::logging::Field{"MNC_FIRST_SEQUENCE",
					std::to_string(
						session.summary.first_sequence)},
				mnc::logging::Field{"MNC_LAST_SEQUENCE",
					std::to_string(
						session.summary.last_sequence)},
				mnc::logging::Field{"MNC_SEQUENCE_GAPS",
					std::to_string(sequence_gaps_)}};
			(void)capture_log.write(mnc::logging::Priority::warning,
				evicted ? "waveform capture incomplete: window "
					  "evicted from history before "
					  "materialization"
					: "waveform capture incomplete: window "
					  "intersects a sequence gap (frames "
					  "lost between PL and daemon)",
				"waveform_session_incomplete", fields);
			continue;
		}
		try {
			enqueue_materialization(session);
		} catch (const std::exception &failure) {
			session.summary.state = WaveformSessionState::incomplete;
			++materialization_failures_;
			const std::array fields{
				mnc::logging::Field{"MNC_WAVEFORM_SESSION",
					std::to_string(session.summary.id)}};
			(void)capture_log.write(mnc::logging::Priority::error,
				std::string("waveform capture incomplete: ") +
					failure.what(),
				"waveform_session_incomplete", fields);
		} catch (...) {
			session.summary.state = WaveformSessionState::incomplete;
			++materialization_failures_;
			const std::array fields{
				mnc::logging::Field{"MNC_WAVEFORM_SESSION",
					std::to_string(session.summary.id)}};
			(void)capture_log.write(mnc::logging::Priority::error,
				"waveform capture incomplete: materialization "
				"could not be queued",
				"waveform_session_incomplete", fields);
		}
	}
}

void WaveformCapture::enqueue_materialization(Session &session)
{
	AsyncWriter::Job job{};
	job.summary = session.summary;
	job.context = session.context;
	job.correlation = correlation_;
	job.correlation_utc_nanoseconds = correlation_utc_nanoseconds_;
	if (job.correlation.tai_nanoseconds == 0u) {
		job.correlation.tai_nanoseconds =
			session.summary.trigger_tai_nanoseconds;
		job.correlation.pl_tick = session.summary.trigger_sequence;
		job.correlation.frame_sequence = session.summary.trigger_sequence;
		job.correlation.uncertainty_nanoseconds = 1'000'000u;
	}
	if (job.correlation_utc_nanoseconds == 0u)
		job.correlation_utc_nanoseconds = translate_clock(
			job.correlation.tai_nanoseconds,
			session.summary.trigger_tai_nanoseconds,
			session.summary.trigger_realtime_nanoseconds);
	job.output_directory = output_directory_;
	job.archive_limit_bytes = archive_limit_bytes_;

	const auto window =
		session.summary.last_sequence -
		session.summary.first_sequence + 1u;
	job.source_frame_count = window;
	if (window > history_.size())
		throw std::runtime_error("waveform session exceeds history");
	/* The acquisition loop performs only a bounded ring snapshot. Decimation,
	 * channel packing, CRC generation, compression, validation, and disk I/O
	 * all run on the background writer. */
	job.frames.reserve(static_cast<std::size_t>(window));
	auto sequence = session.summary.first_sequence;
	auto remaining = window;
	while (remaining != 0u) {
		const auto ring_offset = static_cast<std::size_t>(
			sequence % history_.size());
		const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(
			remaining, history_.size() - ring_offset));
		job.frames.insert(job.frames.end(), history_.begin() +
			static_cast<std::ptrdiff_t>(ring_offset), history_.begin() +
			static_cast<std::ptrdiff_t>(ring_offset + count));
		sequence += count;
		remaining -= count;
	}

	const auto timestamp_at = [&](std::uint64_t sequence,
		std::uint64_t anchor_time) {
		const auto anchor_sequence = job.correlation.frame_sequence;
		const auto rate = static_cast<std::uint64_t>(job.summary.sample_rate_hz);
		if (rate == 0u)
			throw std::runtime_error("waveform sample rate is zero");
		if (sequence >= anchor_sequence) {
			const auto delta = sequence - anchor_sequence;
			const auto nanoseconds = delta * 1'000'000'000ull / rate;
			if (nanoseconds > std::numeric_limits<std::uint64_t>::max() -
					anchor_time)
				throw std::overflow_error("waveform event time overflow");
			return anchor_time + nanoseconds;
		}
		const auto delta = anchor_sequence - sequence;
		const auto nanoseconds = delta * 1'000'000'000ull / rate;
		if (nanoseconds > anchor_time)
			throw std::overflow_error("waveform event time underflow");
		return anchor_time - nanoseconds;
	};
	for (const auto &stored : session.events) {
		auto event = stored.descriptor;
		const auto capture_first = session.summary.first_sequence;
		const auto capture_last = session.summary.last_sequence;
		const auto clamp = [capture_first, capture_last](std::uint64_t value) {
			return std::clamp(value, capture_first, capture_last);
		};
		const bool clipped_start = event.start_sequence < capture_first ||
			event.start_sequence > capture_last;
		event.start_sequence = clamp(event.start_sequence);
		if ((event.flags & mncwf_event_current_valid) != 0u)
			event.current_sequence = clamp(event.current_sequence);
		else {
			event.current_sequence = event.start_sequence;
			event.flags |= mncwf_event_current_valid;
		}
		if ((event.flags & mncwf_event_end_valid) != 0u &&
		    event.end_sequence > capture_last) {
			event.flags &= ~mncwf_event_end_valid;
			event.end_sequence = 0u;
			event.lifecycle = MncwfEventLifecycle::update;
		}
		if ((event.flags & mncwf_event_end_valid) != 0u)
			event.end_sequence = clamp(event.end_sequence);
		if ((event.flags & mncwf_event_trigger_valid) != 0u &&
		    (event.trigger_sequence < capture_first ||
		     event.trigger_sequence > capture_last)) {
			event.flags &= ~mncwf_event_trigger_valid;
			event.trigger_sequence = 0u;
		}
		if (clipped_start) {
			event.flags |= mncwf_event_discontinuous;
			if (event.lifecycle == MncwfEventLifecycle::start)
				event.lifecycle = MncwfEventLifecycle::update;
		}
		if ((event.lifecycle == MncwfEventLifecycle::end ||
		     event.lifecycle == MncwfEventLifecycle::abort ||
		     event.lifecycle == MncwfEventLifecycle::complete) &&
		    (event.flags & mncwf_event_end_valid) == 0u)
			event.lifecycle = MncwfEventLifecycle::update;
		event.start_tai_nanoseconds = timestamp_at(event.start_sequence,
			job.correlation.tai_nanoseconds);
		event.current_tai_nanoseconds = timestamp_at(event.current_sequence,
			job.correlation.tai_nanoseconds);
		event.start_utc_nanoseconds = timestamp_at(event.start_sequence,
			job.correlation_utc_nanoseconds);
		event.current_utc_nanoseconds = timestamp_at(event.current_sequence,
			job.correlation_utc_nanoseconds);
		if ((event.flags & mncwf_event_end_valid) != 0u) {
			event.end_tai_nanoseconds = timestamp_at(event.end_sequence,
				job.correlation.tai_nanoseconds);
			event.end_utc_nanoseconds = timestamp_at(event.end_sequence,
				job.correlation_utc_nanoseconds);
		} else {
			event.end_tai_nanoseconds = 0u;
			event.end_utc_nanoseconds = 0u;
		}
		if ((event.flags & mncwf_event_trigger_valid) != 0u) {
			event.trigger_tai_nanoseconds = timestamp_at(
				event.trigger_sequence, job.correlation.tai_nanoseconds);
			event.trigger_utc_nanoseconds = timestamp_at(
				event.trigger_sequence, job.correlation_utc_nanoseconds);
		} else {
			event.trigger_tai_nanoseconds = 0u;
			event.trigger_utc_nanoseconds = 0u;
		}
		event.flags |= mncwf_event_tai_valid | mncwf_event_utc_valid;
		event.uncertainty_nanoseconds =
			job.correlation.uncertainty_nanoseconds;
		event.duration_samples = (event.flags & mncwf_event_end_valid) != 0u
			? event.end_sequence - event.start_sequence
			: event.current_sequence - event.start_sequence;
		job.events.push_back(std::move(event));
	}

	const auto add_capture_lineage = [&](MncwfLineageRelation relation,
		const MncwfUuid &related) {
		if (uuid_is_zero(related))
			return;
		job.lineage.push_back({relation, 0u, related, {},
			session.summary.first_sequence, session.summary.last_sequence,
			0u, 1u});
	};
	add_capture_lineage(MncwfLineageRelation::previous_continuation,
		session.previous_capture_uuid);
	add_capture_lineage(MncwfLineageRelation::next_continuation,
		session.next_capture_uuid);
	for (const auto &event : job.events)
		job.lineage.push_back({MncwfLineageRelation::event, 0u,
			session.summary.capture_uuid, event.event_uuid,
			event.start_sequence,
			(event.flags & mncwf_event_end_valid) != 0u
				? event.end_sequence : event.current_sequence,
			0u, 1u});
	writer_->enqueue(std::move(job));
	session.materialization_queued = true;
}

void WaveformCapture::collect_materialization_results()
{
	if (!writer_)
		return;
	for (auto &result : writer_->collect()) {
		if (!result.expired_session_ids.empty()) {
			const std::unordered_set<std::uint64_t> expired(
				result.expired_session_ids.begin(),
				result.expired_session_ids.end());
			expired_sessions_ += result.expired_session_ids.size();
			sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(),
				[&expired](const Session &candidate) {
					return expired.contains(candidate.summary.id);
				}), sessions_.end());
			rebuild_indexes();
			rebuild_event_capture_index();
		}
		if (result.success)
			archive_stored_bytes_ = result.archive_stored_bytes;
		if (result.session_id == 0u) {
			if (!result.success) {
				++retention_failures_;
				(void)capture_log.write(mnc::logging::Priority::warning,
					"waveform startup retention failed: " + result.error,
					"waveform_retention_failed");
			}
			continue;
		}
		const auto indexed = session_index_.find(result.session_id);
		const auto session = indexed == session_index_.end()
			? sessions_.end()
			: sessions_.begin() +
				static_cast<std::ptrdiff_t>(indexed->second);
		if (session == sessions_.end())
			continue;
		session->materialization_queued = false;
		if (!result.success) {
			session->summary.state =
				WaveformSessionState::incomplete;
			++materialization_failures_;
			if (result.error.find("archive limit") != std::string::npos)
				++retention_failures_;
			const std::array fields{
				mnc::logging::Field{"MNC_WAVEFORM_SESSION",
					std::to_string(result.session_id)}};
			(void)capture_log.write(mnc::logging::Priority::error,
				"waveform capture file write failed: " +
					result.error,
				"waveform_write_failed", fields);
			continue;
		}
		const std::array fields{
			mnc::logging::Field{"MNC_WAVEFORM_SESSION",
				std::to_string(result.session_id)},
			mnc::logging::Field{"MNC_WAVEFORM_FILE",
				result.filename}};
		(void)capture_log.write(mnc::logging::Priority::notice,
			"waveform capture materialized: " + result.filename,
			"waveform_session_complete", fields);
		std::copy_n(
			result.filename.c_str(),
			std::min(result.filename.size(),
				 session->summary.filename.size() - 1u),
			session->summary.filename.begin());
		session->summary.state = WaveformSessionState::complete;
		session->summary.format_version = result.format_version;
		session->summary.compression = result.compression;
		session->summary.stored_bytes = result.stored_bytes;
		session->summary.logical_sample_bytes =
			result.logical_sample_bytes;
	}
}

WaveformStatus WaveformCapture::status()
{
	collect_discovery_results();
	collect_materialization_results();
	update_transport_status();

	WaveformStatus result{};
	result.running = running() ? 1u : 0u;
	result.active_session = std::any_of(sessions_.begin(), sessions_.end(),
		[](const Session &session) {
			return session.summary.state ==
				       WaveformSessionState::capturing &&
				!session.materialization_queued;
		})
		? 1u
		: 0u;
	result.sample_rate_hz = sample_rate_hz_;
	result.transport_ring_blocks = transport_ring_blocks_;
	result.blocks = blocks_;
	result.frames = frames_;
	result.bytes = bytes_;
	result.invalid_blocks = invalid_blocks_;
	result.sequence_gaps = sequence_gaps_;
	result.transport_overrun_blocks = transport_overrun_blocks_;
	result.materialization_failures = materialization_failures_;
	result.pl_dropped_frames = pl_dropped_frames_;
	result.max_capture_frames = max_capture_frames();
	result.history_oldest_sequence = have_history_ ? oldest_sequence_ : 0u;
	result.history_latest_sequence = have_history_ ? latest_sequence_ : 0u;
	result.history_capacity_frames = history_.size();
	result.archive_limit_bytes = archive_limit_bytes_;
	result.archive_stored_bytes = archive_stored_bytes_;
	result.expired_sessions = expired_sessions_;
	result.retention_failures = retention_failures_;
	result.archive_discovery = archive_discovery_status();
	result.correlation = correlation_;
	for (const auto &session : sessions_) {
		if (session.summary.state == WaveformSessionState::complete)
			++result.completed_sessions;
		else if (session.summary.state == WaveformSessionState::incomplete)
			++result.incomplete_sessions;
	}
	return result;
}

std::vector<WaveformSessionSummary> WaveformCapture::sessions()
{
	return session_page().sessions;
}

WaveformSessionPage WaveformCapture::session_page(
	const WaveformSessionQuery &query)
{
	collect_discovery_results();
	collect_materialization_results();
	std::vector<WaveformSessionSummary> summaries;
	summaries.reserve(sessions_.size());
	for (const auto &session : sessions_)
		summaries.push_back(session.summary);
	return waveform_session_page(summaries, query);
}

std::optional<WaveformSessionSummary>
WaveformCapture::find_session(std::uint64_t session_id)
{
	collect_discovery_results();
	collect_materialization_results();
	const auto session = session_index_.find(session_id);
	if (session == session_index_.end())
		return std::nullopt;
	return sessions_[session->second].summary;
}

std::optional<WaveformSessionSummary>
WaveformCapture::find_session(const MncwfUuid &capture_uuid)
{
	collect_discovery_results();
	collect_materialization_results();
	if (uuid_is_zero(capture_uuid))
		return std::nullopt;
	const auto session = capture_index_.find(mncwf_uuid_string(capture_uuid));
	if (session == capture_index_.end())
		return std::nullopt;
	return sessions_[session->second].summary;
}

} // namespace msap1
