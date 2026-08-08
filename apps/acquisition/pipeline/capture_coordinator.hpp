#pragma once

/**
 * @file capture_coordinator.hpp
 * @brief The acquisition daemon's conductor: pipeline lifecycle,
 *        configuration transactions, and the poll loop.
 */

#include "msap1/acquisition/dma/meter_dma_reader.hpp"
#include "msap1/acquisition/ipc/acquisition_ipc.hpp"
#include "msap1/acquisition/rpu/rpu_controller.hpp"
#include "msap1/meter/measurement_timebase.hpp"
#include "msap1/meter/meter_config.hpp"
#include "msap1/meter/meter_record_stream.hpp"
#include "msap1/settings/settings.hpp"
#include "msap1/waveform/waveform_capture.hpp"
#include "ipc/ipc_channel.hpp"
#include "pipeline/health_monitor.hpp"
#include "pipeline/record_ingestor.hpp"
#include "support/options.hpp"

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
	 * @brief Serve the pipeline until request_stop().
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
	void refresh_rpu_health() { health_.refresh(); }

	/* ── Typed status snapshots for the IPC responses ───────────────── */

	[[nodiscard]] msap1::InfoResponse info_response();
	[[nodiscard]] msap1::CaptureResponse capture_response() const;
	[[nodiscard]] msap1::FrequencyResponse frequency_response() const;
	[[nodiscard]] msap1::DiagnosticResponse diagnostic_response() const;
	[[nodiscard]] msap1::WaveformResponse waveform_response();
	[[nodiscard]] msap1::AdcSourceResponse adc_source_response() const;
	[[nodiscard]] msap1::SimulatorResponse simulator_response() const;

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
	 * CLOCK_REALTIME through the waveform correlation latch; missing the
	 * cadence long enough moves TimeQuality to Holdover on its own.
	 */
	void refresh_time_sync();
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
	msap1::WaveformCapture waveform_;
	msap1::acquisition::RpuController rpu_;
	msap1::MeterRecordStream record_stream_;
	/* UTC mapping for decoded blocks; written by refresh_time_sync() and
	 * read by the ingestor's decode path. Declared before ingest_, which
	 * holds a reference to it. */
	msap1::meter::MeasurementTimebase timebase_;
	std::optional<Clock::time_point> last_time_sync_;
	MeterRecordIngestor ingest_;
	RpuHealthMonitor health_;
	IpcChannel ipc_;
	msap1::AcquisitionCommandRegistry registry_;
	std::atomic<bool> stop_requested_{false};
	bool running_ = false;
	msap1_adc_diagnostic_payload last_adc_diagnostic_{};
};

} // namespace msap1::acquisition::daemon
