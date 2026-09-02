#pragma once

/**
 * @file capture_coordinator.hpp
 * @brief The acquisition daemon's conductor: pipeline lifecycle,
 *        configuration transactions, and the poll loop.
 */

#include "msap1/acquisition/dma/meter_dma_reader.hpp"
#include "msap1/acquisition/ipc/acquisition_ipc.hpp"
#include "msap1/acquisition/rpu/rpu_controller.hpp"
#include "mnc/MeterDataProvider/meter_data_provider.hpp"
#include "msap1/meter/measurement_timebase.hpp"
#include "msap1/meter/meter_config.hpp"
#include "msap1/meter/MeterDataProvider/snapshot/in_process_meter_snapshot_provider.hpp"
#include "msap1/meter/MeterDataProvider/stream/meter_stream_ipc.hpp"
#include "msap1/settings/settings.hpp"
#include "msap1/waveform/waveform_capture.hpp"
#include "ipc/ipc_channel.hpp"
#include "pipeline/aggregation_health_monitor.hpp"
#include "pipeline/event_waveform_linker.hpp"
#include "pipeline/health_monitor.hpp"
#include "pipeline/record_ingestor.hpp"
#include "support/options.hpp"
#include "support/meter_time_control.hpp"

#include <atomic>
#include <cstdint>
#include <optional>
#include <string_view>

namespace msap1::acquisition::daemon {

/**
 * @brief Owns the acquisition pipeline and coordinates every state change.
 *
 * The coordinator is the only owner of the meter DMA, waveform DMA, and the
 * RPU control endpoint, and the only writer of the ACTIVE configuration.
 * Everything it does happens on one thread — the run() poll loop — which is
 * what makes the coordinated transactions below race-free without locks:
 *
 *  - start()/stop(): both DMA consumers arm before the RPU enables capture;
 *    stop is idempotent so a crashed daemon recovers cleanly.
 *  - Configuration changes (settings snapshot, temporary sample rate) are
 *    stop -> configure -> readback-verify -> restart transactions with
 *    rollback to the previous operating point on failure.
 *  - The destructive ADC diagnostic runs only with capture stopped and
 *    restores or recovers the operating point afterwards.
 *
 * Focused concerns are delegated: record ingest and statistics to
 * MeterRecordIngestor, health caching/auditing to RpuHealthMonitor, and
 * socket transport to IpcChannel. Command handlers (ipc/command_handlers)
 * call back into the public API below.
 */
class CaptureCoordinator final {
public:
	explicit CaptureCoordinator(const Options &options);
	~CaptureCoordinator();
	CaptureCoordinator(const CaptureCoordinator &) = delete;
	CaptureCoordinator &operator=(const CaptureCoordinator &) = delete;

	/**
	 * @brief Complete the capture-critical startup sequence.
	 *
	 * Arms both DMA paths, starts RPU capture, and only then binds the IPC
	 * socket. Returning from this method is the daemon's readiness boundary.
	 */
	void initialize();

	/**
	 * @brief Serve an initialized pipeline until request_stop().
	 *
	 * One poll() multiplexes the meter DMA, the waveform DMA, and the IPC
	 * eventfd; the periodic RPU health audit runs between wakeups.
	 *
	 * @throws std::runtime_error when a DMA device disconnects.
	 */
	void run();

	/** @brief Ask run() to return; safe from any thread. */
	void request_stop() noexcept { stop_requested_ = true; }
	/** @brief Re-arm run() before a (re)start. */
	void clear_stop_request() noexcept { stop_requested_ = false; }

	/* ── Pipeline commands (called by IPC command handlers) ─────────── */

	/**
	 * @brief Arm both DMA consumers and start RPU capture.
	 *
	 * @param apply_configuration Commit the coordinated ADC/PL
	 *        configuration before capture starts. The ADC diagnostic
	 *        passes false because a successful flow has already restored
	 *        the operating point itself.
	 */
	void start(bool apply_configuration = true);

	/** @brief Stop RPU capture and release both DMA consumers. */
	void stop() noexcept;

	/** @brief Apply one complete settings snapshot (settings authority). */
	void apply_product_settings(std::string_view json);

	/** @brief Apply a temporary diagnostic sample rate (mnc adc rate). */
	void apply_sample_rate(std::uint32_t sample_rate_hz);

	/** @brief Run the destructive ADC reset diagnostic (flow 1). */
	void run_adc_diagnostic(std::uint32_t flow);

	/** @brief Run an immediate RPU health audit (HealthRefreshRequest). */
	void refresh_rpu_health()
	{
		health_.refresh();
		aggregation_health_.refresh();
	}

	/* ── Typed status snapshots for the IPC responses ───────────────── */

	[[nodiscard]] msap1::InfoResponse info_response();
	[[nodiscard]] msap1::CaptureResponse capture_response() const;
	[[nodiscard]] msap1::FrequencyResponse frequency_response() const;
	[[nodiscard]] msap1::DiagnosticResponse diagnostic_response() const;
	[[nodiscard]] msap1::WaveformResponse waveform_response(
		const msap1::WaveformSessionQuery &query = {});
	[[nodiscard]] msap1::WaveformLookupResponse waveform_lookup_response(
		const msap1::WaveformLookupRequest &request);
	[[nodiscard]] msap1::AdcSourceResponse adc_source_response() const;
	[[nodiscard]] msap1::SimulatorResponse simulator_response() const;
	[[nodiscard]] msap1::SingleCycleResponse single_cycle_response() const;
	[[nodiscard]] msap1::PowerQualityResponse power_quality_response() const;
	[[nodiscard]] msap1::FlickerResponse flicker_response() const;
	[[nodiscard]] msap1::MainsSignalResponse mains_signal_response() const;
	[[nodiscard]] msap1::HarmonicResponse harmonic_response(
		msap1::MeasurementPeriod period) const;
	/**
	 * @brief Drive the simulator's amplitude-envelope sequencer.
	 *
	 * Runs against the LIVE configuration and never restarts capture:
	 * the burst starts on the generator's own half-cycle boundary, so
	 * the programmed amplitude step is the only discontinuity the
	 * metrology engines see.
	 */
	[[nodiscard]] msap1::SimulatorEventResponse simulator_event_response(
		const msap1::SimulatorEventRequest &request);
	[[nodiscard]] msap1::MeterSnapshotResponse meter_snapshot_response(
		const msap1::MeterSnapshotRequest &request) const;

	/* ── State the waveform command handlers need ───────────────────── */

	/** @brief Waveform engine, for trigger/erase commands. */
	[[nodiscard]] msap1::WaveformCapture &waveform() { return waveform_; }
	/** @brief Active settings (waveform trigger duration defaults). */
	[[nodiscard]] const msap1::settings::ProductSettings &
	product_settings() const
	{
		return product_settings_;
	}
	[[nodiscard]] std::uint32_t configuration_generation() const
	{
		return configuration_.wire.generation;
	}

private:
	/** @brief Send the wire configuration and verify the RPU readback. */
	void configure_meter();
	/**
	 * @brief Refresh the measurement-timebase sync point (~10 s cadence).
	 *
	 * Correlates the PL 64-bit conversion sample counter with
	 * CLOCK_REALTIME through the independent meter-time latch, binds the
	 * sync to the ACTIVE configuration (generation + sample rate), and
	 * folds in the kernel clock discipline state (adjtimex). Missing the
	 * cadence long enough moves TimeQuality to Holdover on its own.
	 *
	 * @param force Bypass the cadence limit — used right after a
	 *        successful (re)start so the no-UTC window that follows a
	 *        configuration change stays short.
	 */
	void refresh_time_sync(bool force = false);
	/**
	 * @brief Swap in a staged configuration as one transaction, rolling
	 *        back to the previous operating point on failure.
	 */
	void apply_complete_configuration(
		msap1::PreparedMeterConfiguration staged,
		std::string_view event);
	void service_poll_events();

	Options options_;
	msap1::settings::ProductSettings product_settings_;
	msap1::PreparedMeterConfiguration configuration_;
	msap1::acquisition::MeterDmaReader meter_;
	MeterTimeControl time_control_;
	msap1::WaveformCapture waveform_;
	msap1::acquisition::RpuController rpu_;
	/* UTC mapping for decoded blocks; written by refresh_time_sync() and
	 * read by the ingestor's decode path. Declared before ingest_, which
	 * holds a reference to it. */
	msap1::meter::MeasurementTimebase timebase_;
	std::optional<Clock::time_point> last_time_sync_;
	/* Consecutive TAI/sample correlations provide the measured conversion rate
	 * used by the Class-A ten-second UTC boundary mapper. */
	std::optional<MeterTimeSync> previous_frequency_time_sync_;
	std::uint32_t frequency_10s_boundary_generation_ = 0;
	Frequency10sObserverStatus frequency_10s_observer_status_{};
	/* True after the next UTC ten-minute boundary has been mapped into the
	 * active PL sample-counter epoch. Capture/configuration restarts clear it
	 * and force a fresh mapping. */
	bool ten_minute_boundary_programmed_ = false;
	msap1::meter_stream::MeterRecordStreamClient meter_stream_;
	EventWaveformLinker event_waveform_linker_;
	MeterRecordIngestor ingest_;
	msap1::meter::InProcessMeterSnapshotProvider snapshot_provider_;
	/* Composition root for the two consumer-facing meter-data paths. Narrow
	 * interfaces are still passed to Modbus, historian, and other consumers. */
	mnc::meter::MeterDataProviderView meter_data_provider_;
	RpuHealthMonitor health_;
	AggregationHealthMonitor aggregation_health_;
	IpcChannel ipc_;
	msap1::AcquisitionCommandRegistry registry_;
	std::atomic<bool> stop_requested_{false};
	bool running_ = false;
	msap1_adc_diagnostic_payload last_adc_diagnostic_{};
};

} // namespace msap1::acquisition::daemon
