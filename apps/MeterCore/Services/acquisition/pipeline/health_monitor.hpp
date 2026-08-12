#pragma once

/**
 * @file health_monitor.hpp
 * @brief Cached RPU ADC health with audit scheduling and confirmation policy.
 */

#include "msap1/acquisition/rpu/rpu_controller.hpp"
#include "support/time.hpp"

#include <cstdint>
#include <optional>

namespace msap1::acquisition::daemon {

/**
 * @brief Owns the daemon's cached RPU ADC health snapshot and the policy
 *        that keeps it trustworthy.
 *
 * The RPU register-health audit is expensive (a ~100-register SPI sweep),
 * so consumers (CLI, web) always read this cache; only the monitor talks to
 * the RPU. The policy encoded here:
 *
 *  - Audit on a low-rate schedule (30 s) while capture runs, with a short
 *    settle delay after capture start so the PL rate monitor publishes a
 *    capture-active snapshot first.
 *  - A single failed SPI audit never replaces a known-good cache; the
 *    failure is published only after a confirmation audit (1 s later) also
 *    fails, or immediately when no snapshot exists yet.
 *  - Capture/MeterCore counters in a failed snapshot are still merged into
 *    the cache; only register-derived flags are retained from the last
 *    verified read.
 *  - Health transitions (healthy <-> degraded) are logged once per change,
 *    with the product health-reason codes.
 */
class RpuHealthMonitor final {
public:
	explicit RpuHealthMonitor(msap1::acquisition::RpuController &rpu);

	/**
	 * @brief Run one health audit now and update the cache.
	 *
	 * Used by the HealthRefreshRequest command and by configuration flows
	 * that need immediate post-change confirmation.
	 *
	 * @throws std::runtime_error when the RPU transaction fails; the
	 *         failure is counted and a confirmation audit is scheduled.
	 */
	void refresh();

	/**
	 * @brief Run refresh() when the scheduled audit time has arrived.
	 *
	 * Called from every poll-loop iteration while capture runs; failures
	 * are logged (not thrown) so the loop keeps servicing DMA.
	 */
	void periodic_audit() noexcept;

	/** @brief Reset failure counters and schedule the post-start audit. */
	void on_capture_started();

	/** @brief Leave the startup-settle state when capture stops. */
	void on_capture_stopped();

	/** @brief Last published health snapshot (never a transient failure). */
	[[nodiscard]] const msap1_adc_health_payload &cached() const
	{
		return cached_health_;
	}

	/** @brief True while an audit is outstanding or unconfirmed-failed. */
	[[nodiscard]] bool probe_pending() const
	{
		return stabilizing_ || probe_failures_ != 0u;
	}

	/** @brief Consecutive failed audits since the last verified one. */
	[[nodiscard]] std::uint32_t probe_failures() const
	{
		return probe_failures_;
	}

	/** @brief Milliseconds since the cache was last updated. */
	[[nodiscard]] std::uint32_t health_age_ms() const
	{
		return age_milliseconds(last_health_time_);
	}

private:
	void merge_operational_fields(const msap1_adc_health_payload &health);
	void observe_spi_recovery(const msap1_adc_health_payload &health);
	void observe_health_transition(const msap1_adc_health_payload &health);

	msap1::acquisition::RpuController &rpu_;
	msap1_adc_health_payload cached_health_{};
	bool has_cached_health_ = false;
	std::optional<Clock::time_point> last_health_time_;
	Clock::time_point next_audit_ = Clock::now();
	bool stabilizing_ = false;
	std::uint32_t probe_failures_ = 0;
	std::uint32_t last_spi_retry_recovery_count_ = 0;
	std::optional<std::uint32_t> last_health_flags_;
	std::uint32_t last_spi_error_ = 0;
};

} // namespace msap1::acquisition::daemon
