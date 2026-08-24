#include "pipeline/aggregation_health_monitor.hpp"

#include "support/logs.hpp"

#include <chrono>
#include <exception>
#include <string>
#include <utility>

namespace msap1::acquisition::daemon {
namespace {

using namespace std::chrono_literals;

constexpr auto audit_interval = 5s;
constexpr auto retry_interval = 2s;

bool flag(std::uint32_t flags, std::uint32_t mask)
{
	return (flags & mask) != 0u;
}

} // namespace

AggregationHealthMonitor::AggregationHealthMonitor(std::string service,
						     std::string device)
	: service_(std::move(service)), configured_device_(std::move(device))
{
}

void AggregationHealthMonitor::refresh() noexcept
{
	try {
		if (!controller_)
			controller_ =
				std::make_unique<msap1::acquisition::RpuController>(
					service_, configured_device_);
		const auto health = controller_->query_aggregation_health();
		resolved_device_ = controller_->device_path();
		cached_health_ = health;
		has_cached_health_ = true;
		last_health_time_ = Clock::now();
		probe_failures_ = 0;
		next_audit_ = Clock::now() + audit_interval;
		observe_transition(health);
	} catch (const std::exception &error) {
		++probe_failures_;
		controller_.reset();
		resolved_device_.clear();
		next_audit_ = Clock::now() + retry_interval;
		log_message(aggregation_log, mnc::logging::Priority::warning,
			"R5C1 aggregation health query failed: " +
				std::string(error.what()),
			"aggregation_health_query_failed",
			{{"MNC_CONSECUTIVE_FAILURES",
			  std::to_string(probe_failures_)},
			 {"MNC_RPMSG_SERVICE", service_}});
	}
}

void AggregationHealthMonitor::periodic_audit() noexcept
{
	if (Clock::now() >= next_audit_)
		refresh();
}

void AggregationHealthMonitor::observe_transition(
	const msap1_aggregation_health_payload &health)
{
	if (last_flags_ && *last_flags_ == health.health_flags)
		return;
	const bool authoritative = flag(
		health.health_flags, MSAP1_AGGREGATION_HEALTH_AUTHORITATIVE);
	const bool shadow_healthy = flag(
		health.health_flags, MSAP1_AGGREGATION_HEALTH_TRANSPORT_INITIALIZED) &&
		flag(health.health_flags, MSAP1_AGGREGATION_HEALTH_INPUT_HEALTHY) &&
		health.fifo_errors == 0u && health.ring_overflows == 0u;
	const bool output_healthy =
		flag(health.health_flags, MSAP1_AGGREGATION_HEALTH_ENGINE_READY) &&
		flag(health.health_flags, MSAP1_AGGREGATION_HEALTH_OUTPUT_READY) &&
		flag(health.health_flags, MSAP1_AGGREGATION_HEALTH_OUTPUT_ACTIVE) &&
		health.output_errors == 0u && health.output_drops == 0u;
	const bool healthy = shadow_healthy && (!authoritative || output_healthy);

	log_message(aggregation_log,
		healthy ? mnc::logging::Priority::notice
			: mnc::logging::Priority::warning,
		healthy ? (authoritative ?
			"R5C1 authoritative aggregation health became healthy" :
			"R5C1 shadow aggregation receiver became healthy") :
			"R5C1 aggregation health became degraded",
		healthy ? "aggregation_health_healthy" :
			"aggregation_health_degraded",
		{{"MNC_AGGREGATION_HEALTH_FLAGS",
		  std::to_string(health.health_flags)},
		 {"MNC_FRAMES_RECEIVED", std::to_string(health.frames_received)},
		 {"MNC_RECORDS_EMITTED", std::to_string(health.records_emitted)},
		 {"MNC_FIFO_ERRORS", std::to_string(health.fifo_errors)},
		 {"MNC_OUTPUT_ERRORS", std::to_string(health.output_errors)}});
	last_flags_ = health.health_flags;
}

} // namespace msap1::acquisition::daemon
