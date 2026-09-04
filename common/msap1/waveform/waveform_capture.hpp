#pragma once

#include "msap1/waveform/mncwf_v4.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
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
inline constexpr std::size_t waveform_max_page_sessions = 100;
inline constexpr std::size_t waveform_session_name_size = 96;
inline constexpr std::size_t waveform_persisted_channels = 7;
inline constexpr std::uint64_t waveform_default_archive_limit_bytes =
	8ull * 1024ull * 1024ull * 1024ull;

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

enum class WaveformCompression : std::uint32_t {
	none = 0,
	zstd_chunks = 1,
	mixed_raw_zstd_chunks = 2,
	raw_chunks = 3,
};

enum class WaveformOriginFilter : std::uint32_t {
	all = 0,
	manual = 1,
	power_quality = 2,
};

struct WaveformSessionQuery {
	/** Exclusive descending cursor; zero starts at the newest session. */
	std::uint64_t before_session_id = 0;
	std::uint32_t limit = waveform_max_ipc_sessions;
	WaveformOriginFilter origin = WaveformOriginFilter::all;
};

struct WaveformEventIdentity {
	std::uint64_t session = 0;
	std::uint64_t counter = 0;
	bool operator==(const WaveformEventIdentity &) const = default;
};

enum class WaveformEventLifecycle : std::uint8_t {
	start = 0,
	update = 1,
	end = 2,
	abort = 3,
};

struct WaveformCorrelation {
	std::uint64_t tai_nanoseconds = 0;
	std::uint64_t pl_tick = 0;
	std::uint64_t frame_sequence = 0;
	std::uint64_t uncertainty_nanoseconds = 0;
};

/** Immutable authorities copied into each session when its first trigger lands. */
struct WaveformCaptureContext {
	MncwfV4CaptureMetadata capture_metadata{};
	std::vector<MncwfV4ChannelDefinition> channels;
	MncwfClockSource clock_source = MncwfClockSource::system;
	MncwfTimeQuality time_quality = MncwfTimeQuality::unknown;
	std::uint16_t time_flags = 0;
	std::int32_t utc_offset_seconds = 0;
};

/** Construction limits. Production uses the 128 MiB default; a smaller ring
 * lets deterministic verification exercise capacity rollover without changing
 * the runtime policy or allocating a production-sized test fixture. */
struct WaveformCaptureOptions {
	std::size_t history_capacity_frames = waveform_history_frames;
	/** Independent metrology-time correlation source. Production binds this
	 * to /dev/meter-time; waveform DMA itself never owns time registers. */
	std::function<std::optional<WaveformCorrelation>()> correlation_source;
	/** Optional deterministic test seam invoked immediately before each
	 * persisted file is validated. Production leaves it empty. */
	std::function<void(std::stop_token, const std::filesystem::path &)>
		archive_discovery_hook;
	/** Physical archive ceiling. Enforcement runs only on the background
	 * writer and never deletes an active, temporary, malformed, or incomplete
	 * capture. */
	std::uint64_t archive_limit_bytes = waveform_default_archive_limit_bytes;
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
	/** Bit N is set when at least one trigger from WaveformTriggerSource N
	 * contributed to this (possibly merged) capture session. */
	std::uint32_t trigger_source_mask = 0;
	/** Zero for a master; otherwise the immediately preceding contiguous
	 * session sealed at the 128 MiB safe materialization limit. */
	std::uint64_t continuation_of_session_id = 0;
	/** First session in a continuation chain (equal to id for a master). */
	std::uint64_t master_session_id = 0;
	WaveformSessionState state = WaveformSessionState::capturing;
	/**
	 * Capture-file decimation divisor: each persisted sample is the mean
	 * of this many acquisition frames. Sequences stay in the acquisition
	 * frame domain; the effective file rate is sample_rate_hz/decimation.
	 */
	std::uint32_t decimation = 1;
	std::uint32_t format_version = 0;
	WaveformCompression compression = WaveformCompression::none;
	std::uint64_t stored_bytes = 0;
	std::uint64_t logical_sample_bytes = 0;
	/** Internal edge indicator used to avoid duplicate historian links. */
	bool association_created = false;
	std::array<char, waveform_session_name_size> filename{};
	MncwfUuid capture_uuid{};
};

struct WaveformSessionPage {
	WaveformOriginFilter origin = WaveformOriginFilter::all;
	std::uint32_t limit = waveform_max_ipc_sessions;
	std::uint64_t total_sessions = 0;
	std::uint64_t completed_sessions = 0;
	std::uint64_t incomplete_sessions = 0;
	std::uint64_t active_sessions = 0;
	/** Zero means there is no older matching page. */
	std::uint64_t next_before_session_id = 0;
	std::vector<WaveformSessionSummary> sessions;
};

/** Apply the public descending cursor and trigger-origin filter to summaries. */
[[nodiscard]] WaveformSessionPage waveform_session_page(
	std::span<const WaveformSessionSummary> sessions,
	const WaveformSessionQuery &query = {});

enum class WaveformArchiveDiscoveryState : std::uint32_t {
	not_started = 0,
	scanning = 1,
	complete = 2,
	cancelled = 3,
	failed = 4,
};

/** Progress of the one-shot persisted MNCWF validation pass. */
struct WaveformArchiveDiscoveryStatus {
	WaveformArchiveDiscoveryState state =
		WaveformArchiveDiscoveryState::not_started;
	std::uint64_t scanned_files = 0;
	std::uint64_t total_files = 0;
	std::uint64_t rejected_files = 0;
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
	std::uint64_t archive_limit_bytes = waveform_default_archive_limit_bytes;
	std::uint64_t archive_stored_bytes = 0;
	std::uint64_t expired_sessions = 0;
	std::uint64_t retention_failures = 0;
	WaveformArchiveDiscoveryStatus archive_discovery{};
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
static_assert(sizeof(WaveformTransportStatusIoctl) == 32);

class WaveformCapture {
public:
	explicit WaveformCapture(std::string device_path,
				 std::filesystem::path output_directory,
				 WaveformCaptureContext context = {},
				 WaveformCaptureOptions options = {});
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
				       std::uint32_t decimation,
				       WaveformTriggerSource source);
	/** Merge one stable PQ lifecycle into the active capture union. START and
	 * recovery UPDATE edges add one marker; UPDATE/END extend without duplicate
	 * markers. A session stays open while any linked event remains active. */
	WaveformSessionSummary track_power_quality_event(
		WaveformEventIdentity event_id, WaveformEventLifecycle lifecycle,
		std::uint64_t trigger_sequence, std::uint64_t current_sequence,
		std::uint32_t pretrigger_ms, std::uint32_t posttrigger_ms,
		std::uint32_t decimation, MncwfV4EventDescriptor descriptor);
	/** Replace the authority used by sessions created after this call. Existing
	 * sessions retain their original capture-time snapshot. */
	void set_context(WaveformCaptureContext context);
	void set_archive_limit_gib(std::uint32_t limit_gib);
	void set_time_context(MncwfClockSource source, MncwfTimeQuality quality,
		std::uint16_t leap_flags = 0u) noexcept;
	void erase(std::uint64_t session_id);
	WaveformStatus status();
	/** Compatibility view used by status/trigger replies: newest 16 sessions. */
	std::vector<WaveformSessionSummary> sessions();
	WaveformSessionPage session_page(const WaveformSessionQuery &query = {});
	[[nodiscard]] std::optional<WaveformSessionSummary>
	find_session(std::uint64_t session_id);
	[[nodiscard]] std::optional<WaveformSessionSummary>
	find_session(const MncwfUuid &capture_uuid);

private:
	struct Event;
	struct Session;
	struct EventCaptureState {
		std::uint64_t onset_session_id = 0;
		std::uint64_t recovery_session_id = 0;
		bool terminal = false;
	};
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
	void begin_persisted_session_discovery();
	void discover_persisted_sessions(
		std::stop_token stop, std::vector<std::filesystem::path> files);
	void collect_discovery_results();
	void rebuild_session_lineage();
	void rebuild_indexes();
	void rebuild_event_capture_index();
	[[nodiscard]] WaveformArchiveDiscoveryStatus
	archive_discovery_status() const;
	bool intersects_gap(std::uint64_t first, std::uint64_t last) const;
	std::uint64_t max_capture_frames() const noexcept;
	void update_transport_status() noexcept;
	std::optional<WaveformCorrelation> correlate() const noexcept;

	std::string device_path_;
	std::filesystem::path output_directory_;
	WaveformCaptureContext context_{};
	std::function<std::optional<WaveformCorrelation>()> correlation_source_;
	std::function<void(std::stop_token, const std::filesystem::path &)>
		archive_discovery_hook_;
	mutable std::mutex archive_discovery_mutex_;
	WaveformArchiveDiscoveryStatus archive_discovery_{};
	std::string archive_discovery_error_;
	bool archive_discovery_result_ready_ = false;
	bool archive_discovery_result_collected_ = false;
	bool archive_discovery_error_reported_ = false;
	bool startup_retention_queued_ = false;
	std::jthread archive_discovery_worker_;
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
	std::unordered_map<std::uint64_t, std::size_t> session_index_;
	std::unordered_map<std::string, std::size_t> capture_index_;
	std::unordered_map<std::string, EventCaptureState> event_capture_index_;
	std::vector<Session> discovered_sessions_;
	std::vector<GapRange> gaps_;
	std::unique_ptr<AsyncWriter> writer_;
	WaveformCorrelation correlation_{};
	std::uint64_t correlation_utc_nanoseconds_ = 0;
	std::uint64_t archive_limit_bytes_ = waveform_default_archive_limit_bytes;
	std::uint64_t archive_stored_bytes_ = 0;
	std::uint64_t expired_sessions_ = 0;
	std::uint64_t retention_failures_ = 0;
};

} // namespace msap1
