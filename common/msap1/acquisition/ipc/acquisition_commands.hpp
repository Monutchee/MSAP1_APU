#ifndef MSAP1_ACQUISITION_COMMANDS_HPP
#define MSAP1_ACQUISITION_COMMANDS_HPP

/**
 * Typed MSAP1 acquisition command vocabulary.
 *
 * Every command is one request struct paired with one response struct.  The
 * wire identity is derived from the struct's `command` name, so adding a
 * command means adding a struct here and registering a handler in the daemon;
 * there is no parallel enum or serialization code to keep in sync.  Payloads
 * are glaze BEVE, so struct members are the schema.
 *
 * This header is deliberately pure data: no Boost, no glaze, no transport.
 */

#include "msap1/meter/meter_record.hpp"
#include "msap1/meter/meter_data.hpp"
#include "msap1/meter/meter_timing.hpp"
#include "mnc/MeterDataProvider/snapshot/meter_snapshot.hpp"
#include "msap1/acquisition/rpu/rpu_control_protocol.h"
#include "msap1/waveform/waveform_capture.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

namespace msap1 {

inline constexpr const char *acquisition_socket_path =
	"/run/monutchee/fpga-acquisition.sock";
/* 18: InfoResponse carries the newest 150/180-cycle aggregate (MTR2) record
 * beside the basic latest record.
 * 19: InfoResponse carries the time quality stamped onto that aggregate at
 * ingest, so its provenance no longer follows the daemon's current clock
 * state.
 * 20: Normal meter reads use a product-neutral typed snapshot request;
 * raw MeterRecord remains confined to the legacy diagnostic info reply.
 * 21: Snapshot diagnostics follow the v3 record envelope: the transport
 * drop counters are emit_drops/result_drops and the configured-window echo
 * is replaced by the block's actual sample count.
 * 22: InfoResponse carries the kernel DMA transport counters (produced,
 * consumed, overrun, cyclic callbacks, ring depth) beside the ingest
 * counters, so health output distinguishes a PL-side loss from a kernel
 * ring overrun without reading the device. */
inline constexpr std::uint16_t acquisition_ipc_version = 26;
inline constexpr std::uint32_t meter_record_stale_after_ms = 1000;
inline constexpr std::uint32_t acquisition_age_unavailable =
	std::numeric_limits<std::uint32_t>::max();
inline constexpr std::uint32_t waveform_duration_unspecified =
	std::numeric_limits<std::uint32_t>::max();

/** Frame::message_type is the FNV-1a hash of the request struct's name. */
consteval std::uint32_t acquisition_command_hash(std::string_view name)
{
	std::uint32_t hash = 0x811c9dc5u;
	for (const char character : name) {
		hash ^= static_cast<std::uint8_t>(character);
		hash *= 0x01000193u;
	}
	return hash;
}

template <typename Request>
inline constexpr std::uint32_t acquisition_command_id =
	acquisition_command_hash(Request::command);

enum class AcquisitionStatus : std::uint32_t {
	ok = 0,
	bad_request = 1,
	not_running = 2,
	dma_error = 3,
	rpu_error = 4,
	internal_error = 5,
	configuration_error = 6,
};

/**
 * Fixed-layout hardware-mirror structs (packed C structs shared with the RPU
 * firmware) cross the socket as raw bytes.  Both peers ship in the same image
 * on the same architecture, so the in-memory layout is the wire layout.
 * glaze reflection cannot see through C arrays, and binding references into
 * packed structs is undefined, so the bytes stay opaque until value().
 * Must remain an aggregate so glaze reflection applies to the wrapper.
 */
template <typename T>
struct PackedIpc {
	static_assert(std::is_trivially_copyable_v<T>);
	std::array<std::uint8_t, sizeof(T)> bytes{};

	PackedIpc &operator=(const T &value)
	{
		bytes = std::bit_cast<std::array<std::uint8_t, sizeof(T)>>(value);
		return *this;
	}
	[[nodiscard]] T value() const { return std::bit_cast<T>(bytes); }
};

struct FrequencyIpcConfiguration {
	std::uint32_t enabled = 1;
	std::uint32_t reference_channel = 6;
	std::uint32_t mode = MSAP1_FREQUENCY_MODE_ROLLING_CYCLES;
	std::uint32_t averaging_cycles = 10;
	std::uint32_t averaging_window_ms = 1000;
	std::uint32_t minimum_millihz = 40000;
	std::uint32_t maximum_millihz = 70000;
	std::uint32_t hysteresis_microvolts = 1000000;
};

struct SimulatorIpcChannel {
	double rms = 0.0;
	double phase_degrees = 0.0;
	/* Constant offset in engineering units (volts/amps). */
	double dc = 0.0;
	/* RMS of the uniform white fluctuation the PL adds, engineering
	 * units; 0 keeps the channel noise-free. */
	double noise_rms = 0.0;
};

struct SimulatorIpcConfiguration {
	std::uint32_t frequency_millihz = 60000;
	/* Keep the waveform's phase/framing across the configuration
	 * commit instead of restarting at 0 degrees. */
	std::uint32_t preserve_phase = 0;
	std::array<SimulatorIpcChannel, 8> channels{};
};

struct WaveformSessionIpc {
	std::uint64_t id = 0;
	std::uint64_t trigger_sequence = 0;
	std::uint64_t first_sequence = 0;
	std::uint64_t last_sequence = 0;
	std::uint64_t trigger_tai_nanoseconds = 0;
	std::uint64_t trigger_realtime_nanoseconds = 0;
	std::uint32_t sample_rate_hz = 0;
	std::uint32_t event_count = 0;
	WaveformSessionState state = WaveformSessionState::capturing;
	std::uint32_t decimation = 1;
	std::string filename;
};

/* ---- responses ------------------------------------------------------- */

/** Acquisition/health snapshot shared by info, health, and sample-rate-set. */
struct InfoResponse {
	AcquisitionStatus status = AcquisitionStatus::ok;
	bool running = false;
	bool has_meter_record = false;
	/* Absence of the aggregate is carried exactly like the basic record's:
	 * a presence flag beside a default-constructed record, never a
	 * sentinel inside the record itself. */
	bool has_aggregate_record = false;
	bool health_probe_pending = false;
	std::uint32_t sample_rate_hz = 0;
	std::uint32_t configuration_generation = 0;
	std::uint32_t meter_record_age_ms = acquisition_age_unavailable;
	/* Freshness of latest_aggregate_record only. The aggregate cadence is
	 * ~15x the basic one, so it must never borrow meter_record_age_ms. */
	std::uint32_t aggregate_record_age_ms = acquisition_age_unavailable;
	std::uint32_t rpu_health_age_ms = acquisition_age_unavailable;
	std::uint32_t health_probe_failures = 0;
	std::uint32_t adc_source = MSAP1_ADC_SOURCE_PHYSICAL;
	std::uint64_t meter_records = 0;
	std::uint64_t dma_bytes = 0;
	std::uint64_t dma_read_errors = 0;
	std::uint64_t invalid_records = 0;
	std::uint64_t sequence_gaps = 0;
	/* Kernel DMA transport accounting, sampled as a whole at reply time.
	 * These are the driver's own totals, not ingest counters: they say
	 * what the ring produced and how much of it userspace took, which is
	 * what separates a PL-side loss from a consumer stall. Grouped by
	 * meaning rather than by width — the ring depth belongs with the
	 * counters it bounds. Observability only. */
	std::uint64_t transport_produced_blocks = 0;
	std::uint64_t transport_consumed_blocks = 0;
	std::uint64_t transport_overrun_blocks = 0;
	std::uint64_t transport_callbacks = 0;
	std::uint32_t transport_ring_blocks = 0;
	/* UTC synchronization state of the measurement timebase RIGHT NOW,
	 * at the moment this reply was built. Reported beside — never inside
	 * — the electrical health fields. It describes the daemon's live
	 * clock state, so it is never the provenance of a past measurement:
	 * use aggregate_time_quality for the cached aggregate. */
	msap1::meter::TimeQuality time_quality =
		msap1::meter::TimeQuality::Unsynchronized;
	/* UTC synchronization state that applied when latest_aggregate_record
	 * was INGESTED, stamped onto its decoded AggregateTiming at decode
	 * time. Meaningful only when has_aggregate_record is set. Carried on
	 * the wire because the raw 256-byte PL record holds no UTC state:
	 * re-decoding the cached bytes cannot recover it. */
	msap1::meter::TimeQuality aggregate_time_quality =
		msap1::meter::TimeQuality::Unsynchronized;
	PackedIpc<msap1_adc_health_payload> rpu_health{};
	MeterRecord latest_record{};
	/* Newest 150/180-cycle aggregate (MTR2, 0x00020002). Meaningful only
	 * when has_aggregate_record is set; the basic latest_record above is
	 * unaffected by, and never replaced with, an aggregate. */
	MeterRecord latest_aggregate_record{};
};

/** Per-channel diagnostics that are specific to today's MTR1 record. */
struct MeterChannelDiagnostics {
	std::int64_t mean_micro_units = 0;
	std::uint32_t rms_count = 0;
};

/** Frequency status/counters that accompany the typed frequency reading. */
struct MeterFrequencyDiagnostics {
	bool enabled = false;
	bool reference_valid = false;
	bool out_of_range = false;
	bool timed_out = false;
	bool arithmetic_error = false;
	std::uint32_t period_q16_samples = 0;
	std::uint32_t measurement_sequence = 0;
	std::uint32_t mode = 0;
	std::uint32_t reference_channel = 0;
	std::uint32_t cycles_used = 0;
};

/**
 * Compatibility timing carried beside the v20 snapshot response.
 *
 * New consumers should use MeterSnapshot::timing, which is stamped once by
 * record ingestion and therefore cannot change merely because a request is
 * served later.  This compact MSAP1 field remains for existing diagnostics
 * and is populated from that same stored provenance.
 */
struct MeterBlockTimingIpc {
	std::uint64_t block_sequence = 0;
	std::uint64_t first_sample_index = 0;
	std::uint32_t sample_count = 0;
	std::uint32_t cycle_count = 0;
	std::uint32_t nominal_frequency_hz = 0;
	bool cycle_locked = false;
	bool free_run_fallback = false;
	msap1::meter::TimeQuality time_quality =
		msap1::meter::TimeQuality::Unsynchronized;
};

/**
 * Product diagnostics intentionally kept outside mnc::meter::MeterSnapshot.
 * The snapshot is the reusable measurement API; these counters describe the
 * current MSAP1 PL transport and may evolve independently.
 */
struct MeterSnapshotDiagnostics {
	std::uint32_t sample_rate_hz = 0;
	/* Actual samples in the record's block (envelope word 6) — a
	 * cycle-defined block, not a configured-window echo. */
	std::uint32_t block_sample_count = 0;
	std::uint32_t status = 0;
	std::uint32_t capture_frames = 0;
	std::uint32_t header_errors = 0;
	std::uint32_t fifo_overflows = 0;
	std::uint32_t emit_drops = 0;
	std::uint32_t result_drops = 0;
	std::array<MeterChannelDiagnostics, 8> channels{};
	MeterFrequencyDiagnostics frequency{};
	std::optional<MeterBlockTimingIpc> timing;
};

struct MeterSnapshotResponse {
	AcquisitionStatus status = AcquisitionStatus::ok;
	bool running = false;
	bool has_snapshot = false;
	mnc::meter::MeterSnapshot snapshot{};
	MeterSnapshotDiagnostics diagnostics{};
};

struct CaptureResponse {
	AcquisitionStatus status = AcquisitionStatus::ok;
	bool running = false;
};

struct FrequencyResponse {
	AcquisitionStatus status = AcquisitionStatus::ok;
	std::uint32_t configuration_generation = 0;
	FrequencyIpcConfiguration frequency{};
};

struct DiagnosticResponse {
	AcquisitionStatus status = AcquisitionStatus::ok;
	bool running = false;
	std::uint32_t live_drdy_frequency_hz = 0;
	PackedIpc<msap1_adc_diagnostic_payload> diagnostic{};
};

struct WaveformResponse {
	AcquisitionStatus status = AcquisitionStatus::ok;
	WaveformStatus waveform{};
	std::vector<WaveformSessionIpc> sessions;
};

struct AdcSourceResponse {
	AcquisitionStatus status = AcquisitionStatus::ok;
	bool running = false;
	std::uint32_t adc_source = MSAP1_ADC_SOURCE_PHYSICAL;
	std::uint32_t configuration_generation = 0;
	std::uint32_t health_flags = 0;
};

struct SimulatorResponse {
	AcquisitionStatus status = AcquisitionStatus::ok;
	std::uint32_t adc_source = MSAP1_ADC_SOURCE_PHYSICAL;
	std::uint32_t configuration_generation = 0;
	std::uint32_t health_flags = 0;
	std::uint32_t simulator_active_generation = 0;
	std::uint32_t simulator_frame_count = 0;
	std::uint32_t simulator_saturation_count = 0;
	std::uint32_t simulator_missed_sample_count = 0;
	SimulatorIpcConfiguration simulator{};
};

struct ApplyResponse {
	AcquisitionStatus status = AcquisitionStatus::ok;
	std::uint32_t configuration_generation = 0;
};

struct SingleCycleResponse {
	AcquisitionStatus status = AcquisitionStatus::ok;
	bool running = false;
	bool has_snapshot = false;
	std::uint64_t records = 0;
	msap1::SingleCycleSnapshot snapshot{};
};

/* ---- requests --------------------------------------------------------- */

struct InfoRequest {
	static constexpr std::string_view command = "info";
	using Response = InfoResponse;
	std::uint16_t version = acquisition_ipc_version;
};

/** Latest-state meter projection. This path is lossy by design. */
struct MeterSnapshotRequest {
	static constexpr std::string_view command = "meter-snapshot";
	using Response = MeterSnapshotResponse;
	std::uint16_t version = acquisition_ipc_version;
	mnc::meter::MeterSnapshotRequest selection{};
};

/** Returns the daemon's cached health.  Web polling must never trigger a
 * 100-register SPI audit. */
struct HealthRequest {
	static constexpr std::string_view command = "health";
	using Response = InfoResponse;
	std::uint16_t version = acquisition_ipc_version;
};

/** Runs an immediate RPU register-health audit before answering. */
struct HealthRefreshRequest {
	static constexpr std::string_view command = "health-refresh";
	using Response = InfoResponse;
	std::uint16_t version = acquisition_ipc_version;
};

struct StartRequest {
	static constexpr std::string_view command = "capture-start";
	using Response = CaptureResponse;
	std::uint16_t version = acquisition_ipc_version;
};

struct StopRequest {
	static constexpr std::string_view command = "capture-stop";
	using Response = CaptureResponse;
	std::uint16_t version = acquisition_ipc_version;
};

struct FrequencyGetRequest {
	static constexpr std::string_view command = "frequency-configuration-get";
	using Response = FrequencyResponse;
	std::uint16_t version = acquisition_ipc_version;
};

/** Applies a temporary sample rate; the persistent rate lives in settings. */
struct SampleRateSetRequest {
	static constexpr std::string_view command = "sample-rate-set";
	using Response = InfoResponse;
	std::uint16_t version = acquisition_ipc_version;
	std::uint32_t sample_rate_hz = 0;
};

struct DiagnosticRunRequest {
	static constexpr std::string_view command = "adc-diagnostic-run";
	using Response = DiagnosticResponse;
	std::uint16_t version = acquisition_ipc_version;
	std::uint32_t flow = 0;
};

struct WaveformStatusRequest {
	static constexpr std::string_view command = "waveform-status";
	using Response = WaveformResponse;
	std::uint16_t version = acquisition_ipc_version;
};

struct WaveformListRequest {
	static constexpr std::string_view command = "waveform-list";
	using Response = WaveformResponse;
	std::uint16_t version = acquisition_ipc_version;
};

struct WaveformTriggerRequest {
	static constexpr std::string_view command = "waveform-trigger";
	using Response = WaveformResponse;
	std::uint16_t version = acquisition_ipc_version;
	std::uint32_t pretrigger_ms = waveform_duration_unspecified;
	std::uint32_t posttrigger_ms = waveform_duration_unspecified;
	/** Capture-file decimation divisor; 0 selects the persisted default. */
	std::uint32_t decimation = 0;
	WaveformTriggerSource source = WaveformTriggerSource::manual_cli;
};

struct WaveformDeleteRequest {
	static constexpr std::string_view command = "waveform-delete";
	using Response = WaveformResponse;
	std::uint16_t version = acquisition_ipc_version;
	std::uint64_t session_id = 0;
};

struct AdcSourceGetRequest {
	static constexpr std::string_view command = "adc-source-get";
	using Response = AdcSourceResponse;
	std::uint16_t version = acquisition_ipc_version;
};

struct SimulatorGetRequest {
	static constexpr std::string_view command = "adc-simulator-get";
	using Response = SimulatorResponse;
	std::uint16_t version = acquisition_ipc_version;
};

/** Latest single-cycle diagnostic snapshot (SCYC records, roadmap M3). */
struct SingleCycleRequest {
	static constexpr std::string_view command = "meter-single-cycle";
	using Response = SingleCycleResponse;
	std::uint16_t version = acquisition_ipc_version;
};

/** Applies one complete settings snapshot (sent by the settings service). */
struct ConfigurationApplyRequest {
	static constexpr std::string_view command = "configuration-apply";
	using Response = ApplyResponse;
	std::uint16_t version = acquisition_ipc_version;
	std::string configuration_json;
};

/** Every command; keeps the compile-time hash-collision check exhaustive. */
using AcquisitionCommandList = std::tuple<
	InfoRequest, MeterSnapshotRequest, HealthRequest, HealthRefreshRequest, StartRequest,
	StopRequest, FrequencyGetRequest, SampleRateSetRequest,
	DiagnosticRunRequest, WaveformStatusRequest, WaveformListRequest,
	WaveformTriggerRequest, WaveformDeleteRequest, AdcSourceGetRequest,
	SimulatorGetRequest, SingleCycleRequest, ConfigurationApplyRequest>;

namespace detail {

template <typename... Requests>
consteval bool command_ids_are_unique(std::tuple<Requests...> *)
{
	const std::array ids{acquisition_command_id<Requests>...};
	for (std::size_t first = 0; first < ids.size(); ++first)
		for (std::size_t second = first + 1; second < ids.size();
		     ++second)
			if (ids[first] == ids[second])
				return false;
	return true;
}

} // namespace detail

static_assert(detail::command_ids_are_unique(
		      static_cast<AcquisitionCommandList *>(nullptr)),
	      "acquisition command name hashes collide; rename a command");

} // namespace msap1

#endif
