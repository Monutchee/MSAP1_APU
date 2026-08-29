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

msap1_demand_config_ack_payload AggregationHealthMonitor::configure_demand(
	const msap1_demand_config_payload &configuration)
{
	desired_demand_ = configuration;
	try {
		if (!controller_)
			controller_ =
				std::make_unique<msap1::acquisition::RpuController>(
					service_, configured_device_);
		const auto acknowledgement = msap1::decode_demand_config_ack(
			controller_->transact(MSAP1_RPU_MSG_DEMAND_CONFIG_SET,
				&configuration, sizeof(configuration), 1000ms));
		if (acknowledgement.method != configuration.method ||
		    acknowledgement.window_seconds != configuration.window_seconds ||
		    acknowledgement.update_seconds != configuration.update_seconds)
			throw std::runtime_error(
				"R5C1 demand configuration readback does not match");
		resolved_device_ = controller_->device_path();
		demand_profile_generation_ = acknowledgement.profile_generation;
		return acknowledgement;
	} catch (...) {
		controller_.reset();
		resolved_device_.clear();
		throw;
	}
}

void AggregationHealthMonitor::refresh() noexcept
{
	try {
		if (!controller_)
			controller_ =
				std::make_unique<msap1::acquisition::RpuController>(
					service_, configured_device_);
		const auto health = controller_->query_aggregation_health();
		if (desired_demand_)
			(void)configure_demand(*desired_demand_);
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
	const bool authoritative = flag(
		health.health_flags, MSAP1_AGGREGATION_HEALTH_AUTHORITATIVE);
	const bool shadow_healthy = flag(
		health.health_flags, MSAP1_AGGREGATION_HEALTH_TRANSPORT_INITIALIZED) &&
		flag(health.health_flags, MSAP1_AGGREGATION_HEALTH_INPUT_HEALTHY) &&
		health.fifo_errors == 0u && health.ring_overflows == 0u &&
		health.software_ring_push_failures == 0u &&
		health.input_records_dropped == 0u &&
		health.software_ring_pressure <
			MSAP1_AGGREGATION_RING_PRESSURE_CRITICAL;
	const bool output_healthy =
		flag(health.health_flags, MSAP1_AGGREGATION_HEALTH_ENGINE_READY) &&
		flag(health.health_flags, MSAP1_AGGREGATION_HEALTH_OUTPUT_READY) &&
		flag(health.health_flags, MSAP1_AGGREGATION_HEALTH_OUTPUT_ACTIVE) &&
		health.output_errors == 0u && health.output_drops == 0u;
	const bool healthy = shadow_healthy && (!authoritative || output_healthy);
	if (last_flags_ && *last_flags_ == health.health_flags &&
	    last_healthy_ && *last_healthy_ == healthy)
		return;

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
		 {"MNC_OUTPUT_ERRORS", std::to_string(health.output_errors)},
		 {"MNC_SOFTWARE_RING_CURRENT",
		  std::to_string(health.software_ring_current)},
		 {"MNC_SOFTWARE_RING_HIGH_WATER",
		  std::to_string(health.software_ring_high_water)},
		 {"MNC_SOFTWARE_RING_CAPACITY",
		  std::to_string(health.software_ring_capacity)},
		 {"MNC_SOFTWARE_RING_PRESSURE",
		  std::to_string(health.software_ring_pressure)},
		 {"MNC_SOFTWARE_RING_PUSH_FAILURES",
		  std::to_string(health.software_ring_push_failures)},
		 {"MNC_INPUT_RECORDS_DROPPED",
		  std::to_string(health.input_records_dropped)},
		 {"MNC_HARDWARE_FIFO_CURRENT_WORDS",
		  std::to_string(health.hardware_fifo_current_words)},
		 {"MNC_HARDWARE_FIFO_HIGH_WATER_WORDS",
		  std::to_string(health.hardware_fifo_high_water_words)},
		 {"MNC_HARDWARE_FIFO_FULL_EVENTS",
		  std::to_string(health.hardware_fifo_full_events)},
		 {"MNC_INPUT_MAX_BATCH",
		  std::to_string(health.input_max_batch)},
		 {"MNC_INPUT_MAX_RUNTIME_US",
		  std::to_string(health.input_max_runtime_us)},
		 {"MNC_VALIDATOR_MAX_SCHEDULE_GAP_US",
		  std::to_string(health.validator_max_schedule_gap_us)}});
	last_flags_ = health.health_flags;
	last_healthy_ = healthy;
}

} // namespace msap1::acquisition::daemon
