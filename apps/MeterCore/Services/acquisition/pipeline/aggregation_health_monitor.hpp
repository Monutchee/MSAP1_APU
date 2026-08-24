#pragma once

/**
 * @file aggregation_health_monitor.hpp
 * @brief Optional, cached R5C1 aggregation-offload diagnostics.
 */

#include "msap1/acquisition/rpu/rpu_controller.hpp"
#include "support/time.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace msap1::acquisition::daemon {

/**
 * Polls R5C1 independently from the ADC-owning R5C0 endpoint.
 *
 * R5C1 is optional during the shadow-validation stage. Endpoint discovery and
 * request failures are therefore cached as diagnostics and never stop DMA or
 * acquisition startup. Dropping the controller after a failure also lets a
 * restarted remoteproc be rediscovered on the next audit.
 */
class AggregationHealthMonitor final {
public:
	AggregationHealthMonitor(std::string service, std::string device);

	/** Perform one best-effort audit; failures are retained, never thrown. */
	void refresh() noexcept;
	/** Perform the audit when its low-rate deadline has arrived. */
	void periodic_audit() noexcept;

	[[nodiscard]] bool has_cached_health() const noexcept
	{
		return has_cached_health_;
	}
	[[nodiscard]] const msap1_aggregation_health_payload &cached() const
	{
		return cached_health_;
	}
	[[nodiscard]] bool probe_pending() const noexcept
	{
		return !has_cached_health_ || probe_failures_ != 0u;
	}
	[[nodiscard]] std::uint32_t probe_failures() const noexcept
	{
		return probe_failures_;
	}
	[[nodiscard]] std::uint32_t health_age_ms() const
	{
		return age_milliseconds(last_health_time_);
	}
	[[nodiscard]] const std::string &device_path() const noexcept
	{
		return resolved_device_;
	}

private:
	void observe_transition(const msap1_aggregation_health_payload &health);

	std::string service_;
	std::string configured_device_;
	std::string resolved_device_;
	std::unique_ptr<msap1::acquisition::RpuController> controller_;
	msap1_aggregation_health_payload cached_health_{};
	bool has_cached_health_ = false;
	std::optional<Clock::time_point> last_health_time_;
	Clock::time_point next_audit_ = Clock::now();
	std::uint32_t probe_failures_ = 0;
	std::optional<std::uint32_t> last_flags_;
};

} // namespace msap1::acquisition::daemon
