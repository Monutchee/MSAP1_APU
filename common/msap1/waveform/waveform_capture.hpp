#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace msap1 {

inline constexpr std::uint32_t waveform_block_magic = 0x314d4657u; // WFM1
inline constexpr std::uint32_t waveform_block_version = 0x00010000u;
inline constexpr std::size_t waveform_channels = 8;
inline constexpr std::size_t waveform_frame_bytes = 32;
inline constexpr std::size_t waveform_frames_per_block = 1024;
inline constexpr std::size_t waveform_block_header_bytes = 64;
inline constexpr std::size_t waveform_block_bytes =
	waveform_block_header_bytes +
	waveform_frames_per_block * waveform_frame_bytes;
inline constexpr std::size_t waveform_history_bytes = 128u * 1024u * 1024u;
inline constexpr std::size_t waveform_history_frames =
	waveform_history_bytes / waveform_frame_bytes;
inline constexpr std::size_t waveform_max_ipc_sessions = 16;
inline constexpr std::size_t waveform_session_name_size = 96;
inline constexpr std::size_t waveform_persisted_channels = 7;

enum class WaveformChannelKind : std::uint32_t {
	current = 1,
	voltage = 2,
	debug = 3,
};

/*
 * A persisted capture retains exact ADC counts. This descriptor provides the
 * profile-specific transform used by readers:
 *
 *   engineering units = raw * scale_micro_units_q16 / (65536 * 1,000,000)
 *
 * Keeping the transform beside the samples permits raw/converted display
 * without duplicating every waveform frame.
 */
struct WaveformChannelMetadata {
	std::uint32_t source_channel = 0;
	WaveformChannelKind kind = WaveformChannelKind::debug;
	std::uint32_t scale_micro_units_q16 = 0;
	std::uint32_t flags = 0;
	std::array<char, 8> name{};
	std::array<char, 8> unit{};
};

enum class WaveformTriggerSource : std::uint32_t {
	manual_cli = 1,
	manual_web = 2,
	pq_event = 3,
};

enum class WaveformSessionState : std::uint32_t {
	capturing = 1,
	complete = 2,
	incomplete = 3,
};

struct WaveformCorrelation {
	std::uint64_t tai_nanoseconds = 0;
	std::uint64_t pl_tick = 0;
	std::uint64_t frame_sequence = 0;
	std::uint64_t uncertainty_nanoseconds = 0;
};

/*
 * One correlation of the PL conversion-domain sample counter with
 * CLOCK_REALTIME, produced for the measurement timebase (UTC mapping).
 * The PL latches the counter atomically inside the correlation ioctl; the
 * CLOCK_REALTIME bracket around that ioctl bounds the latch instant, so the
 * midpoint is the estimate and the bracket width is the uncertainty. The
 * caller adds the PL elasticity-FIFO offset bound, which depends on the
 * active sample rate.
 */
struct WaveformTimeSync {
	std::uint64_t sample_counter = 0;
	std::uint64_t realtime_nanoseconds = 0;
	std::uint64_t bracket_nanoseconds = 0;
};

struct WaveformSessionSummary {
	std::uint64_t id = 0;
	std::uint64_t trigger_sequence = 0;
	std::uint64_t first_sequence = 0;
	std::uint64_t last_sequence = 0;
	std::uint64_t trigger_tai_nanoseconds = 0;
	std::uint64_t trigger_realtime_nanoseconds = 0;
	std::uint32_t sample_rate_hz = 0;
	std::uint32_t event_count = 0;
	WaveformSessionState state = WaveformSessionState::capturing;
	std::uint32_t reserved = 0;
	std::array<char, waveform_session_name_size> filename{};
};

struct WaveformStatus {
	std::uint32_t running = 0;
	std::uint32_t active_session = 0;
	std::uint32_t sample_rate_hz = 0;
	std::uint32_t transport_ring_blocks = 0;
	std::uint64_t blocks = 0;
	std::uint64_t frames = 0;
	std::uint64_t bytes = 0;
	std::uint64_t invalid_blocks = 0;
	std::uint64_t sequence_gaps = 0;
	std::uint64_t transport_overrun_blocks = 0;
	std::uint64_t materialization_failures = 0;
	/**
	 * Latest cumulative PL-side drop counter, copied from the most recent
	 * WFM1 block header. Non-zero means the PL elasticity FIFO overflowed
	 * because the DMA stopped draining it — loss upstream of the kernel
	 * transport, invisible to transport_overrun_blocks.
	 */
	std::uint32_t pl_dropped_frames = 0;
	/**
	 * Largest pre+post trigger window trigger() currently accepts, in
	 * frames. Rate-dependent: the history ring is sized in frames, so its
	 * span in seconds shrinks as the sample rate rises.
	 */
	std::uint64_t max_capture_frames = 0;
	std::uint64_t history_oldest_sequence = 0;
	std::uint64_t history_latest_sequence = 0;
	std::uint64_t history_capacity_frames = waveform_history_frames;
	std::uint64_t completed_sessions = 0;
	std::uint64_t incomplete_sessions = 0;
	WaveformCorrelation correlation{};
};

#pragma pack(push, 1)
struct WaveformBlockHeader {
	std::uint32_t magic;
	std::uint32_t version;
	std::uint32_t block_bytes;
	std::uint32_t frame_count;
	std::uint32_t frame_bytes;
	std::uint32_t first_sequence_low;
	std::uint32_t first_sequence_high;
	std::uint32_t first_tick_low;
	std::uint32_t first_tick_high;
	std::uint32_t measured_sample_rate_hz;
	std::uint32_t configuration_generation;
	std::uint32_t status;
	std::uint32_t dropped_frames;
	std::uint32_t block_sequence;
	std::uint32_t reserved0;
	std::uint32_t reserved1;

	std::uint64_t first_sequence() const noexcept
	{
		return (static_cast<std::uint64_t>(first_sequence_high) << 32u) |
			first_sequence_low;
	}

	std::uint64_t first_tick() const noexcept
	{
		return (static_cast<std::uint64_t>(first_tick_high) << 32u) |
			first_tick_low;
	}
};

struct WaveformBlock {
	WaveformBlockHeader header;
	std::array<std::array<std::int32_t, waveform_channels>,
		   waveform_frames_per_block>
		frames;
};

struct WaveformCorrelationIoctl {
	std::uint64_t tai_before_nanoseconds;
	std::uint64_t tai_after_nanoseconds;
	std::uint64_t pl_tick;
	std::uint64_t frame_sequence;
};

struct WaveformTransportStatusIoctl {
	std::uint64_t produced_blocks;
	std::uint64_t consumed_blocks;
	std::uint64_t overrun_blocks;
	std::uint32_t ring_blocks;
	/* Cyclic completion callbacks the driver saw; diagnostic only. */
	std::uint32_t callbacks;
};
#pragma pack(pop)

static_assert(sizeof(WaveformBlockHeader) == waveform_block_header_bytes);
static_assert(sizeof(WaveformBlock) == waveform_block_bytes);

class WaveformCapture {
public:
	explicit WaveformCapture(std::string device_path,
				 std::filesystem::path output_directory,
				 std::array<WaveformChannelMetadata,
					    waveform_persisted_channels>
					 channel_metadata = {});
	~WaveformCapture();

	WaveformCapture(const WaveformCapture &) = delete;
	WaveformCapture &operator=(const WaveformCapture &) = delete;

	void start();
	void stop() noexcept;
	int fd() const noexcept { return fd_; }
	bool running() const noexcept { return fd_ >= 0; }

	void read_available();
	WaveformSessionSummary trigger(std::uint32_t pretrigger_ms,
				       std::uint32_t posttrigger_ms,
				       WaveformTriggerSource source);
	void erase(std::uint64_t session_id);
	WaveformStatus status();
	std::vector<WaveformSessionSummary> sessions();

	/**
	 * Sample one PL-counter/CLOCK_REALTIME correlation for the UTC
	 * measurement timebase. Available whenever the waveform device is
	 * open (a capture session is not required); nullopt when the device
	 * is closed or the correlation read fails.
	 */
	std::optional<WaveformTimeSync> time_sync() const noexcept;

private:
	struct Event;
	struct Session;
	struct AsyncWriter;
	struct GapRange {
		std::uint64_t first = 0;
		std::uint64_t last = 0;
	};

	void accept_block(const WaveformBlock &block);
	void begin_stream_epoch() noexcept;
	void finish_sessions();
	void enqueue_materialization(Session &session);
	void collect_materialization_results();
	void discover_persisted_sessions();
	bool intersects_gap(std::uint64_t first, std::uint64_t last) const;
	std::uint64_t max_capture_frames() const noexcept;
	void update_transport_status() noexcept;
	std::optional<WaveformCorrelation> correlate() const noexcept;

	std::string device_path_;
	std::filesystem::path output_directory_;
	std::array<WaveformChannelMetadata, waveform_persisted_channels>
		channel_metadata_{};
	bool persisted_sessions_discovered_ = false;
	int fd_ = -1;
	std::vector<std::array<std::int32_t, waveform_channels>> history_;
	bool have_history_ = false;
	std::uint64_t oldest_sequence_ = 0;
	std::uint64_t latest_sequence_ = 0;
	std::uint64_t blocks_ = 0;
	std::uint64_t frames_ = 0;
	std::uint64_t bytes_ = 0;
	std::uint64_t invalid_blocks_ = 0;
	std::uint64_t sequence_gaps_ = 0;
	std::uint64_t transport_overrun_blocks_ = 0;
	std::uint64_t transport_last_overrun_blocks_ = 0;
	std::uint64_t materialization_failures_ = 0;
	std::uint32_t transport_ring_blocks_ = 0;
	std::uint32_t pl_dropped_frames_ = 0;
	std::uint32_t sample_rate_hz_ = 0;
	std::optional<std::uint32_t> configuration_generation_;
	std::uint64_t next_session_id_ = 1;
	std::vector<Session> sessions_;
	std::vector<GapRange> gaps_;
	std::unique_ptr<AsyncWriter> writer_;
	WaveformCorrelation correlation_{};
};

} // namespace msap1
