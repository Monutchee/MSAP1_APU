#include "msap1/waveform_capture.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace msap1 {
namespace {

constexpr unsigned long waveform_correlate_ioctl =
	_IOR('W', 0x01, WaveformCorrelationIoctl);

[[noreturn]] void throw_errno(const std::string &operation)
{
	throw std::runtime_error(operation + ": " + std::strerror(errno));
}

std::uint64_t tai_now_nanoseconds()
{
	timespec timestamp{};
	if (::clock_gettime(CLOCK_TAI, &timestamp) != 0)
		throw_errno("read CLOCK_TAI");
	return static_cast<std::uint64_t>(timestamp.tv_sec) * 1000000000ull +
		static_cast<std::uint64_t>(timestamp.tv_nsec);
}

template <typename T>
void write_binary(std::ofstream &stream, const T &value)
{
	stream.write(reinterpret_cast<const char *>(&value), sizeof(value));
}

} // namespace

struct WaveformCapture::Event {
	std::uint64_t sequence = 0;
	std::uint64_t tai_nanoseconds = 0;
	WaveformTriggerSource source = WaveformTriggerSource::manual_cli;
};

struct WaveformCapture::Session {
	WaveformSessionSummary summary{};
	std::vector<Event> events;
};

WaveformCapture::WaveformCapture(std::string device_path,
				 std::filesystem::path output_directory)
	: device_path_(std::move(device_path)),
	  output_directory_(std::move(output_directory)),
	  history_(waveform_history_frames)
{
}

WaveformCapture::~WaveformCapture()
{
	stop();
}

void WaveformCapture::start()
{
	if (fd_ >= 0)
		return;
	fd_ = ::open(device_path_.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd_ < 0)
		throw_errno("open " + device_path_);
	std::filesystem::create_directories(output_directory_);
	if (const auto current = correlate())
		correlation_ = *current;
}

void WaveformCapture::stop() noexcept
{
	if (fd_ >= 0)
		::close(fd_);
	fd_ = -1;
	for (auto &session : sessions_) {
		if (session.summary.state == WaveformSessionState::capturing)
			session.summary.state = WaveformSessionState::incomplete;
	}
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

void WaveformCapture::read_available()
{
	std::array<WaveformBlock, 2> blocks{};
	for (;;) {
		const auto size = ::read(fd_, blocks.data(), sizeof(blocks));
		if (size < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
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
	finish_sessions();
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

	const auto first = header.first_sequence();
	if (have_history_ && first != latest_sequence_ + 1u) {
		if (first > latest_sequence_ + 1u)
			sequence_gaps_ += first - (latest_sequence_ + 1u);
		else
			++sequence_gaps_;
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
	sample_rate_hz_ = header.measured_sample_rate_hz;
	++blocks_;
	frames_ += waveform_frames_per_block;
}

WaveformSessionSummary WaveformCapture::trigger(
	std::uint32_t pretrigger_ms, std::uint32_t posttrigger_ms,
	WaveformTriggerSource source)
{
	if (!have_history_ || sample_rate_hz_ == 0)
		throw std::runtime_error("waveform history is not ready");
	if (pretrigger_ms > 120000u || posttrigger_ms > 120000u)
		throw std::invalid_argument("waveform duration exceeds 120 seconds");

	const auto frames_for = [this](std::uint32_t milliseconds) {
		return (static_cast<std::uint64_t>(sample_rate_hz_) * milliseconds +
			999u) /
			1000u;
	};
	const auto anchor = latest_sequence_;
	const auto requested_first =
		anchor > frames_for(pretrigger_ms)
			? anchor - frames_for(pretrigger_ms)
			: 0u;
	const auto requested_last = anchor + frames_for(posttrigger_ms);
	/*
	 * Refresh the PL-tick/CLOCK_TAI mapping at every trigger. A startup-only
	 * correlation would accumulate oscillator drift during long uptimes and
	 * make an otherwise precise trigger timestamp less useful.
	 */
	if (const auto current = correlate())
		correlation_ = *current;
	const auto now_tai = tai_now_nanoseconds();

	auto active = std::find_if(sessions_.begin(), sessions_.end(),
		[](const Session &session) {
			return session.summary.state ==
				WaveformSessionState::capturing;
		});
	if (active == sessions_.end() ||
	    requested_first > active->summary.last_sequence + 1u) {
		Session session{};
		session.summary.id = next_session_id_++;
		session.summary.trigger_sequence = anchor;
		session.summary.first_sequence = requested_first;
		session.summary.last_sequence = requested_last;
		session.summary.trigger_tai_nanoseconds = now_tai;
		session.summary.sample_rate_hz = sample_rate_hz_;
		session.summary.state = WaveformSessionState::capturing;
		sessions_.push_back(std::move(session));
		active = std::prev(sessions_.end());
	} else {
		active->summary.first_sequence =
			std::min(active->summary.first_sequence, requested_first);
		active->summary.last_sequence =
			std::max(active->summary.last_sequence, requested_last);
	}
	active->events.push_back({anchor, now_tai, source});
	active->summary.event_count =
		static_cast<std::uint32_t>(active->events.size());
	return active->summary;
}

void WaveformCapture::finish_sessions()
{
	for (auto &session : sessions_) {
		if (session.summary.state != WaveformSessionState::capturing ||
		    latest_sequence_ < session.summary.last_sequence)
			continue;
		if (session.summary.first_sequence < oldest_sequence_) {
			session.summary.state = WaveformSessionState::incomplete;
			continue;
		}
		materialize(session);
	}
}

void WaveformCapture::materialize(Session &session)
{
	std::ostringstream name;
	name << "waveform-" << session.summary.id << "-"
	     << session.summary.trigger_tai_nanoseconds << ".mncwf";
	const auto path = output_directory_ / name.str();
	const auto temporary = path.string() + ".tmp";
	std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
	if (!output)
		throw std::runtime_error("create waveform file " + temporary);

	const std::array<char, 8> magic{'M', 'N', 'C', 'W', 'F', '1', '\0', '\0'};
	output.write(magic.data(), magic.size());
	const std::uint32_t version = 1;
	const std::uint32_t header_bytes = 128;
	write_binary(output, version);
	write_binary(output, header_bytes);
	write_binary(output, session.summary.id);
	write_binary(output, session.summary.first_sequence);
	write_binary(output, session.summary.last_sequence);
	write_binary(output, session.summary.trigger_sequence);
	write_binary(output, session.summary.trigger_tai_nanoseconds);
	write_binary(output, session.summary.sample_rate_hz);
	const auto event_count =
		static_cast<std::uint32_t>(session.events.size());
	write_binary(output, event_count);
	write_binary(output, correlation_.tai_nanoseconds);
	write_binary(output, correlation_.pl_tick);
	write_binary(output, correlation_.frame_sequence);
	write_binary(output, correlation_.uncertainty_nanoseconds);
	std::array<std::byte, 32> reserved{};
	output.write(reinterpret_cast<const char *>(reserved.data()),
		     reserved.size());

	for (const auto &event : session.events) {
		write_binary(output, event.sequence);
		write_binary(output, event.tai_nanoseconds);
		const auto source = static_cast<std::uint32_t>(event.source);
		write_binary(output, source);
		const std::uint32_t event_reserved = 0;
		write_binary(output, event_reserved);
	}
	for (auto sequence = session.summary.first_sequence;
	     sequence <= session.summary.last_sequence; ++sequence) {
		const auto &frame = history_[sequence % history_.size()];
		output.write(reinterpret_cast<const char *>(frame.data()),
			     sizeof(frame));
	}
	output.close();
	if (!output)
		throw std::runtime_error("write waveform file " + temporary);
	std::filesystem::rename(temporary, path);

	const auto filename = name.str();
	std::copy_n(filename.c_str(),
		    std::min(filename.size(),
			     session.summary.filename.size() - 1u),
		    session.summary.filename.begin());
	session.summary.state = WaveformSessionState::complete;
}

WaveformStatus WaveformCapture::status() const noexcept
{
	WaveformStatus result{};
	result.running = running() ? 1u : 0u;
	result.active_session = std::any_of(sessions_.begin(), sessions_.end(),
		[](const Session &session) {
			return session.summary.state ==
				WaveformSessionState::capturing;
		})
		? 1u
		: 0u;
	result.sample_rate_hz = sample_rate_hz_;
	result.blocks = blocks_;
	result.frames = frames_;
	result.bytes = bytes_;
	result.invalid_blocks = invalid_blocks_;
	result.sequence_gaps = sequence_gaps_;
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

std::vector<WaveformSessionSummary> WaveformCapture::sessions() const
{
	std::vector<WaveformSessionSummary> result;
	const auto count =
		std::min(sessions_.size(), waveform_max_ipc_sessions);
	result.reserve(count);
	for (std::size_t offset = 0; offset < count; ++offset)
		result.push_back(sessions_[sessions_.size() - 1u - offset].summary);
	return result;
}

} // namespace msap1
