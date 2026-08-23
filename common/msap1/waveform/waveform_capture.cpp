#include "msap1/waveform/waveform_capture.hpp"

#include "mnc/logging/logging.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <cstring>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace msap1 {
namespace {

constexpr unsigned long waveform_correlate_ioctl =
	_IOR('W', 0x01, WaveformCorrelationIoctl);
constexpr unsigned long waveform_transport_status_ioctl =
	_IOR('W', 0x02, WaveformTransportStatusIoctl);
constexpr unsigned long waveform_ten_minute_boundary_ioctl =
	_IOW('W', 0x03, WaveformTenMinuteBoundaryIoctl);

/*
 * Session outcomes are journaled here, where the state transitions happen:
 * half of them (gap intersection, epoch reset) never reach the writer, so
 * they otherwise leave no trace anywhere — a capture could fail with every
 * failure counter still reading zero.
 */
const mnc::logging::Logger capture_log{"fpga-acquisition", "waveform"};

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

template <typename T>
void write_binary(std::ofstream &stream, const T &value)
{
	stream.write(reinterpret_cast<const char *>(&value), sizeof(value));
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

struct WaveformCapture::Event {
	std::uint64_t sequence = 0;
	std::uint64_t tai_nanoseconds = 0;
	WaveformTriggerSource source = WaveformTriggerSource::manual_cli;
};

struct WaveformCapture::Session {
	WaveformSessionSummary summary{};
	std::vector<Event> events;
	bool materialization_queued = false;
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
		std::vector<Event> events;
		WaveformCorrelation correlation{};
		std::filesystem::path output_directory;
		std::array<WaveformChannelMetadata,
			   waveform_persisted_channels>
			channel_metadata{};
		std::vector<Frame> frames;
	};

	struct Result {
		std::uint64_t session_id = 0;
		bool success = false;
		std::string filename;
		std::string error;
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
	static Result write(const Job &job)
	{
		Result result{};
		result.session_id = job.summary.id;

		std::ostringstream name;
		name << "waveform-" << job.summary.id << "-"
		     << filename_timestamp(
				job.summary.trigger_realtime_nanoseconds)
		     << ".mncwf";
		result.filename = name.str();
		const auto path = job.output_directory / result.filename;
		const auto temporary = path.string() + ".tmp";

		try {
			std::ofstream output(
				temporary, std::ios::binary | std::ios::trunc);
			if (!output)
				throw std::runtime_error(
					"create waveform file " + temporary);

			WaveformFileHeaderV2 header{};
			header.magic =
				{'M', 'N', 'C', 'W', 'F', '1', '\0', '\0'};
			header.session_id = job.summary.id;
			header.first_sequence = job.summary.first_sequence;
			header.last_sequence = job.summary.last_sequence;
			header.trigger_sequence = job.summary.trigger_sequence;
			header.trigger_tai_nanoseconds =
				job.summary.trigger_tai_nanoseconds;
			header.sample_rate_hz = job.summary.sample_rate_hz;
			header.event_count =
				static_cast<std::uint32_t>(job.events.size());
			header.correlation_tai_nanoseconds =
				job.correlation.tai_nanoseconds;
			header.correlation_pl_tick = job.correlation.pl_tick;
			header.correlation_frame_sequence =
				job.correlation.frame_sequence;
			header.correlation_uncertainty_nanoseconds =
				job.correlation.uncertainty_nanoseconds;
			header.event_table_offset =
				header.channel_table_offset +
				sizeof(job.channel_metadata);
			header.frame_data_offset =
				header.event_table_offset +
				job.events.size() * 24u;
			header.frame_count = job.frames.size();
			header.trigger_realtime_nanoseconds =
				job.summary.trigger_realtime_nanoseconds;
			header.decimation =
				std::max<std::uint32_t>(1u, job.summary.decimation);
			output.write(reinterpret_cast<const char *>(&header),
				     sizeof(header));
			output.write(
				reinterpret_cast<const char *>(
					job.channel_metadata.data()),
				sizeof(job.channel_metadata));

			for (const auto &event : job.events) {
				write_binary(output, event.sequence);
				write_binary(output, event.tai_nanoseconds);
				const auto source =
					static_cast<std::uint32_t>(event.source);
				write_binary(output, source);
				const std::uint32_t event_reserved = 0;
				write_binary(output, event_reserved);
			}
			for (const auto &frame : job.frames) {
				output.write(
					reinterpret_cast<const char *>(
						frame.data()),
					waveform_persisted_channels *
						sizeof(std::int32_t));
			}
			output.close();
			if (!output)
				throw std::runtime_error(
					"write waveform file " + temporary);
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
				 std::array<WaveformChannelMetadata,
					    waveform_persisted_channels>
					 channel_metadata)
	: device_path_(std::move(device_path)),
	  output_directory_(std::move(output_directory)),
	  channel_metadata_(std::move(channel_metadata)),
	  history_(waveform_history_frames),
	  writer_(std::make_unique<AsyncWriter>())
{
	static constexpr std::array<const char *, waveform_persisted_channels>
		default_names{"Ia", "Ib", "Ic", "In", "Vc", "Vb", "Va"};
	for (std::size_t channel = 0; channel < channel_metadata_.size();
	     ++channel) {
		auto &metadata = channel_metadata_[channel];
		if (metadata.name.front() != '\0')
			continue;
		metadata.source_channel = static_cast<std::uint32_t>(channel);
		metadata.kind = channel < 4u ? WaveformChannelKind::current
					   : WaveformChannelKind::voltage;
		std::copy_n(default_names[channel],
			    std::min(std::strlen(default_names[channel]),
				     metadata.name.size() - 1u),
			    metadata.name.begin());
		const char *unit = channel < 4u ? "A" : "V";
		std::copy_n(unit, 1u, metadata.unit.begin());
	}
}

WaveformCapture::~WaveformCapture()
{
	stop();
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
	discover_persisted_sessions();
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
}

void WaveformCapture::discover_persisted_sessions()
{
	if (persisted_sessions_discovered_)
		return;
	persisted_sessions_discovered_ = true;

	std::error_code error;
	if (!std::filesystem::exists(output_directory_, error))
		return;

	for (const auto &entry :
	     std::filesystem::directory_iterator(output_directory_, error)) {
		if (error)
			break;
		if (!entry.is_regular_file() ||
		    entry.path().extension() != ".mncwf")
			continue;

		std::ifstream input(entry.path(), std::ios::binary);
		WaveformFileHeaderV2 header{};
		input.read(reinterpret_cast<char *>(&header), sizeof(header));
		const auto bytes_read = static_cast<std::size_t>(input.gcount());
		if (input.gcount() < 64 ||
		    header.magic !=
			    std::array<char, 8>{
				    'M', 'N', 'C', 'W', 'F', '1', '\0', '\0'} ||
		    (header.version != 1u && header.version != 2u &&
		     header.version != 3u))
			continue;

		/* v2 wrote zeros where v3 keeps the decimation divisor. */
		const std::uint32_t decimation =
			header.version >= 3u ? header.decimation : 1u;
		if (!valid_decimation(decimation))
			continue;

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
		session.summary.state = WaveformSessionState::complete;

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
				    static_cast<std::uint64_t>(
					    header.event_count) *
					    24u &&
		    header.last_sequence >= header.first_sequence &&
		    (header.last_sequence - header.first_sequence) %
			    decimation == 0u &&
		    header.frame_count ==
			    (header.last_sequence - header.first_sequence) /
					    decimation + 1u) {
			expected_file_bytes =
				header.frame_data_offset +
				header.frame_count * header.frame_bytes;
			session.summary.trigger_realtime_nanoseconds =
				header.trigger_realtime_nanoseconds;
		} else if (header.version == 1u &&
			   header.header_bytes == 128u &&
			   header.last_sequence >= header.first_sequence) {
			const auto frame_count =
				header.last_sequence - header.first_sequence + 1u;
			expected_file_bytes =
				128u +
				static_cast<std::uint64_t>(
					header.event_count) *
					24u +
				frame_count * waveform_frame_bytes;
			const auto modified =
				entry.last_write_time(error);
			if (!error) {
				const auto system_time =
					std::chrono::system_clock::now() +
					(modified -
					 std::filesystem::file_time_type::
						 clock::now());
				session.summary.trigger_realtime_nanoseconds =
					static_cast<std::uint64_t>(
						std::chrono::duration_cast<
							std::chrono::nanoseconds>(
							system_time.time_since_epoch())
							.count());
			}
		} else {
			continue;
		}
		const auto file_bytes = entry.file_size(error);
		if (error || file_bytes != expected_file_bytes) {
			error.clear();
			continue;
		}

		const auto filename = entry.path().filename().string();
		std::copy_n(filename.c_str(),
			    std::min(filename.size(),
				     session.summary.filename.size() - 1u),
			    session.summary.filename.begin());
		sessions_.push_back(std::move(session));
		next_session_id_ =
			std::max(next_session_id_, header.session_id + 1u);
	}
	std::sort(sessions_.begin(), sessions_.end(),
		[](const Session &left, const Session &right) {
			return left.summary.id < right.summary.id;
		});
}

std::optional<WaveformCorrelation> WaveformCapture::correlate() const noexcept
{
	if (fd_ < 0)
		return std::nullopt;
	WaveformCorrelationIoctl sample{};
	if (::ioctl(fd_, waveform_correlate_ioctl, &sample) != 0)
		return std::nullopt;
	const auto midpoint =
		sample.tai_before_nanoseconds +
		(sample.tai_after_nanoseconds - sample.tai_before_nanoseconds) / 2u;
	return WaveformCorrelation{
		midpoint,
		sample.pl_tick,
		sample.frame_sequence,
		sample.tai_after_nanoseconds - sample.tai_before_nanoseconds,
	};
}

std::optional<WaveformTimeSync> WaveformCapture::time_sync() const noexcept
{
	if (fd_ < 0)
		return std::nullopt;
	/*
	 * The kernel brackets the atomic PL latch with CLOCK_TAI reads; a
	 * CLOCK_REALTIME bracket around the whole ioctl bounds the same latch
	 * instant in the UTC domain, which is what the measurement timebase
	 * maps to. The `frame_sequence` correlation field carries the PL
	 * 64-bit conversion-domain sample counter.
	 */
	std::uint64_t realtime_before = 0;
	std::uint64_t realtime_after = 0;
	WaveformCorrelationIoctl sample{};
	try {
		realtime_before = realtime_now_nanoseconds();
		if (::ioctl(fd_, waveform_correlate_ioctl, &sample) != 0)
			return std::nullopt;
		realtime_after = realtime_now_nanoseconds();
	} catch (...) {
		return std::nullopt;
	}
	return WaveformTimeSync{
		sample.frame_sequence,
		realtime_before + (realtime_after - realtime_before) / 2u,
		realtime_after - realtime_before,
	};
}

void WaveformCapture::program_ten_minute_boundary(
	std::uint64_t target_sample_index, bool valid)
{
	if (fd_ < 0)
		throw std::runtime_error(
			"program ten-minute boundary: waveform device is closed");
	WaveformTenMinuteBoundaryIoctl request{
		target_sample_index,
		valid ? 1u : 0u,
		0u,
	};
	if (::ioctl(fd_, waveform_ten_minute_boundary_ioctl, &request) != 0)
		throw_errno("program ten-minute boundary");
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

	auto active = std::find_if(sessions_.begin(), sessions_.end(),
		[](const Session &session) {
			return session.summary.state ==
				       WaveformSessionState::capturing &&
				!session.materialization_queued;
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
		session.summary.state = WaveformSessionState::capturing;
		sessions_.push_back(std::move(session));
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
	active->events.push_back({anchor, now_tai, source});
	active->summary.event_count =
		static_cast<std::uint32_t>(active->events.size());
	return active->summary;
}

void WaveformCapture::erase(std::uint64_t session_id)
{
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
		std::error_code error;
		(void)std::filesystem::remove(output_directory_ / relative, error);
		if (error)
			throw std::runtime_error(
				"delete waveform file " + filename + ": " +
				error.message());
	}
	sessions_.erase(session);
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
	job.events = session.events;
	job.correlation = correlation_;
	job.output_directory = output_directory_;
	job.channel_metadata = channel_metadata_;

	const auto window =
		session.summary.last_sequence -
		session.summary.first_sequence + 1u;
	if (window > history_.size())
		throw std::runtime_error("waveform session exceeds history");
	/*
	 * Decimation by mean: each stored frame folds `decimation` raw frames
	 * into their per-channel average — a crude anti-alias filter suited
	 * to dip/swell inspection (transient hunting captures at 1). The
	 * session's stored range is trimmed to whole groups so the persisted
	 * invariant frame_count == (last - first) / decimation + 1 is exact.
	 */
	const std::uint64_t decimation =
		std::max<std::uint32_t>(1u, session.summary.decimation);
	const auto output_count = (window + decimation - 1u) / decimation;
	session.summary.last_sequence = session.summary.first_sequence +
		(output_count - 1u) * decimation;
	job.summary = session.summary;
	job.frames.reserve(static_cast<std::size_t>(output_count));
	for (std::uint64_t output = 0; output < output_count; ++output) {
		const auto group_first =
			session.summary.first_sequence + output * decimation;
		const auto group_frames = std::min<std::uint64_t>(
			decimation,
			session.summary.first_sequence + window - group_first);
		std::array<std::int64_t, waveform_channels> sums{};
		for (std::uint64_t offset = 0; offset < group_frames; ++offset) {
			const auto &frame =
				history_[(group_first + offset) % history_.size()];
			for (std::size_t channel = 0;
			     channel < waveform_channels; ++channel)
				sums[channel] += frame[channel];
		}
		AsyncWriter::Frame frame{};
		for (std::size_t channel = 0; channel < waveform_channels;
		     ++channel)
			frame[channel] = static_cast<std::int32_t>(
				sums[channel] /
				static_cast<std::int64_t>(group_frames));
		job.frames.push_back(frame);
	}
	writer_->enqueue(std::move(job));
	session.materialization_queued = true;
}

void WaveformCapture::collect_materialization_results()
{
	if (!writer_)
		return;
	for (auto &result : writer_->collect()) {
		const auto session = std::find_if(
			sessions_.begin(), sessions_.end(),
			[&result](const Session &candidate) {
				return candidate.summary.id == result.session_id;
			});
		if (session == sessions_.end())
			continue;
		session->materialization_queued = false;
		if (!result.success) {
			session->summary.state =
				WaveformSessionState::incomplete;
			++materialization_failures_;
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
	}
}

WaveformStatus WaveformCapture::status()
{
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
	collect_materialization_results();
	std::vector<WaveformSessionSummary> result;
	const auto count =
		std::min(sessions_.size(), waveform_max_ipc_sessions);
	result.reserve(count);
	for (std::size_t offset = 0; offset < count; ++offset)
		result.push_back(sessions_[sessions_.size() - 1u - offset].summary);
	return result;
}

} // namespace msap1
