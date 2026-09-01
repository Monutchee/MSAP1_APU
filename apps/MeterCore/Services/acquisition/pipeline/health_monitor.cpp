#include "pipeline/health_monitor.hpp"

#include "support/logs.hpp"

#include <chrono>
#include <exception>
#include <string>

namespace msap1::acquisition::daemon {

namespace {

using namespace std::chrono_literals;

constexpr auto audit_interval = 30s;
constexpr auto confirmation_interval = 1s;
/*
 * The PL DCLK/DRDY monitor publishes one-second snapshots. Delay the first
 * post-start audit long enough for a complete capture-active window to
 * replace the zero-rate snapshot produced while capture was stopped.
 */
constexpr auto startup_settle_interval = 2s;
constexpr std::uint32_t failures_before_degraded = 2;

} // namespace

RpuHealthMonitor::RpuHealthMonitor(msap1::acquisition::RpuController &rpu)
	: rpu_(rpu)
{
}

void RpuHealthMonitor::refresh()
{
	stabilizing_ = false;
	msap1_adc_health_payload health{};
	try {
		health = rpu_.query_health();
	} catch (...) {
		++probe_failures_;
		next_audit_ = Clock::now() + confirmation_interval;
		throw;
	}
	if (health.adc_source == MSAP1_ADC_SOURCE_SIMULATOR) {
		/* Simulator health has no SPI component; publish directly. */
		probe_failures_ = 0;
		cached_health_ = health;
		has_cached_health_ = true;
		last_health_time_ = Clock::now();
		next_audit_ = Clock::now() + audit_interval;
		observe_health_transition(health);
		return;
	}
	const bool spi_snapshot_valid =
		(health.health_flags & MSAP1_ADC_HEALTH_SPI_RESPONSIVE) != 0u &&
		health.spi_error == MSAP1_ADC_SPI_HEALTH_OK;

	/*
	 * A sweep can report success and still carry a corrupted register
	 * value. The reply's protocol header and its data byte are separate
	 * bytes of one transfer: the RPU validates the header and retries,
	 * but a corruption confined to the data byte leaves a valid header,
	 * so the read succeeds and the wrong value reaches the health flags.
	 * Only the register-derived flags can be falsified that way, and
	 * only losing one is consequential -- gaining one back is always
	 * good news. Treat a lost flag exactly like a transport failure and
	 * make the next audit agree before it reaches the cache; otherwise
	 * one bad byte publishes a degraded ADC that was never degraded.
	 */
	constexpr std::uint32_t confirmable_health_flags =
		MSAP1_ADC_HEALTH_INIT_COMPLETE | MSAP1_ADC_HEALTH_CONFIG_MATCH;
	const bool lost_verified_flag =
		has_cached_health_ &&
		((cached_health_.health_flags & ~health.health_flags) &
		 confirmable_health_flags) != 0u;

	if (spi_snapshot_valid && !lost_verified_flag) {
		probe_failures_ = 0;
		cached_health_ = health;
		has_cached_health_ = true;
		last_health_time_ = Clock::now();
		next_audit_ = Clock::now() + audit_interval;
		observe_spi_recovery(health);
		observe_health_transition(health);
		return;
	}

	++probe_failures_;
	next_audit_ = Clock::now() + confirmation_interval;
	if (probe_failures_ < failures_before_degraded) {
		log_message(
			health_log, mnc::logging::Priority::notice,
			spi_snapshot_valid ?
				"ADC register health flag lost on an otherwise clean SPI sweep; confirmation scheduled" :
				"transient ADC SPI health audit failure; confirmation scheduled",
			"rpu_health_confirmation_pending",
			{{"MNC_CONSECUTIVE_FAILURES",
			  std::to_string(probe_failures_)},
			 {"MNC_SPI_ERROR", std::to_string(health.spi_error)},
			 {"MNC_SPI_PROTOCOL_ERRORS",
			  std::to_string(health.spi_protocol_error_count)},
			 {"MNC_SPI_CONFIG_MISMATCHES",
			  std::to_string(health.spi_config_read_mismatch_count)},
			 {"MNC_SPI_REGISTER",
			  std::to_string(health.spi_last_failed_register)},
			 {"MNC_SPI_RECEIVED_HEADER",
			  std::to_string(health.spi_last_received_header)}});
	}

	/*
	 * A single bad SPI audit must not replace a known-good cache. Publish
	 * the failure after confirmation, or immediately when no previous
	 * snapshot exists during startup.
	 */
	if (has_cached_health_)
		merge_operational_fields(health);
	if (!has_cached_health_ ||
	    probe_failures_ >= failures_before_degraded) {
		cached_health_ = health;
		has_cached_health_ = true;
		last_health_time_ = Clock::now();
		observe_health_transition(health);
	}
}

void RpuHealthMonitor::periodic_audit() noexcept
{
	if (Clock::now() < next_audit_)
		return;
	try {
		refresh();
	} catch (const std::exception &error) {
		log_message(rpmsg_log, mnc::logging::Priority::warning,
			"RPU health query failed: " + std::string(error.what()),
			"health_query_failed",
			{{"MNC_CONSECUTIVE_FAILURES",
			  std::to_string(probe_failures_)}});
	}
}

void RpuHealthMonitor::on_capture_started()
{
	probe_failures_ = 0;
	stabilizing_ = true;
	next_audit_ = Clock::now() + startup_settle_interval;
}

void RpuHealthMonitor::on_capture_stopped()
{
	stabilizing_ = false;
}

void RpuHealthMonitor::on_meter_configuration_applied(
	const msap1_meter_config_payload &configuration,
	const msap1_meter_config_ack_payload &acknowledgement)
{
	cached_health_.meter_generation = acknowledgement.generation;
	cached_health_.conversion_status = acknowledgement.conversion_status;
	cached_health_.processing_status = acknowledgement.processing_status;
	cached_health_.meter_requested_current_adc_phase_map =
		configuration.current_adc_phase_map;
	cached_health_.meter_requested_current_adc_invert_mask =
		configuration.current_adc_invert_mask;
	cached_health_.meter_active_current_adc_phase_map =
		acknowledgement.active_current_adc_phase_map;
	cached_health_.meter_active_current_adc_invert_mask =
		acknowledgement.active_current_adc_invert_mask;
	cached_health_.meter_wiring_apply_status =
		MSAP1_METER_WIRING_APPLY_SUCCESS;
	cached_health_.meter_health_flags &= ~(
		MSAP1_METER_HEALTH_CORES_PRESENT |
		MSAP1_METER_HEALTH_CONFIGURED |
		MSAP1_METER_HEALTH_GENERATION_MATCH |
		MSAP1_METER_HEALTH_ENABLED |
		MSAP1_METER_HEALTH_REMOVE_DC |
		MSAP1_METER_HEALTH_CURRENT_WIRING_MATCH);
	cached_health_.meter_health_flags |=
		MSAP1_METER_HEALTH_CORES_PRESENT |
		MSAP1_METER_HEALTH_CONFIGURED |
		MSAP1_METER_HEALTH_GENERATION_MATCH |
		MSAP1_METER_HEALTH_CURRENT_WIRING_MATCH;
	if ((configuration.flags & MSAP1_METER_CONFIG_ENABLE) != 0u)
		cached_health_.meter_health_flags |= MSAP1_METER_HEALTH_ENABLED;
	if ((configuration.flags & MSAP1_METER_CONFIG_REMOVE_DC) != 0u)
		cached_health_.meter_health_flags |= MSAP1_METER_HEALTH_REMOVE_DC;
	last_health_time_ = Clock::now();
}

void RpuHealthMonitor::on_meter_configuration_failed(bool rollback_succeeded)
{
	cached_health_.meter_wiring_apply_status = rollback_succeeded ?
		MSAP1_METER_WIRING_APPLY_ROLLED_BACK :
		MSAP1_METER_WIRING_APPLY_ROLLBACK_FAILED;
	last_health_time_ = Clock::now();
}

void RpuHealthMonitor::merge_operational_fields(
	const msap1_adc_health_payload &health)
{
	/*
	 * Capture and MeterCore status do not depend on the failed SPI
	 * snapshot. Keep these fast fields current while retaining the last
	 * verified register-derived flags and register bytes.
	 */
	constexpr std::uint32_t register_health_flags =
		MSAP1_ADC_HEALTH_SPI_RESPONSIVE |
		MSAP1_ADC_HEALTH_INIT_COMPLETE |
		MSAP1_ADC_HEALTH_CONFIG_MATCH;
	cached_health_.health_flags =
		(cached_health_.health_flags & register_health_flags) |
		(health.health_flags & ~register_health_flags);
	cached_health_.meter_health_flags = health.meter_health_flags;
	cached_health_.meter_generation = health.meter_generation;
	cached_health_.conversion_status = health.conversion_status;
	cached_health_.processing_status = health.processing_status;
	cached_health_.meter_requested_current_adc_phase_map =
		health.meter_requested_current_adc_phase_map;
	cached_health_.meter_requested_current_adc_invert_mask =
		health.meter_requested_current_adc_invert_mask;
	cached_health_.meter_active_current_adc_phase_map =
		health.meter_active_current_adc_phase_map;
	cached_health_.meter_active_current_adc_invert_mask =
		health.meter_active_current_adc_invert_mask;
	cached_health_.meter_wiring_apply_status =
		health.meter_wiring_apply_status;
	cached_health_.meter_wiring_readback_mismatch_count =
		health.meter_wiring_readback_mismatch_count;
	cached_health_.sample_rate_hz = health.sample_rate_hz;
	cached_health_.capture_flags = health.capture_flags;
	cached_health_.frame_count = health.frame_count;
	cached_health_.overflow_count = health.overflow_count;
	cached_health_.header_error_count = health.header_error_count;
	cached_health_.alert_count = health.alert_count;
	cached_health_.packet_count = health.packet_count;
	cached_health_.dclk_frequency_hz = health.dclk_frequency_hz;
	cached_health_.drdy_frequency_hz = health.drdy_frequency_hz;
	cached_health_.spi_protocol_error_count =
		health.spi_protocol_error_count;
	cached_health_.spi_retry_recovery_count =
		health.spi_retry_recovery_count;
	cached_health_.spi_config_read_mismatch_count =
		health.spi_config_read_mismatch_count;
	cached_health_.spi_last_failed_register =
		health.spi_last_failed_register;
	cached_health_.spi_last_received_header =
		health.spi_last_received_header;
	for (std::size_t bucket = 0;
	     bucket < sizeof(cached_health_.spi_header_histogram) /
			      sizeof(cached_health_.spi_header_histogram[0]);
	     ++bucket)
		cached_health_.spi_header_histogram[bucket] =
			health.spi_header_histogram[bucket];
}

void RpuHealthMonitor::observe_spi_recovery(
	const msap1_adc_health_payload &health)
{
	if (health.spi_retry_recovery_count == last_spi_retry_recovery_count_)
		return;
	log_message(health_log, mnc::logging::Priority::notice,
		"ADC SPI register read recovered after retry",
		"spi_retry_recovered",
		{{"MNC_SPI_RETRY_RECOVERIES",
		  std::to_string(health.spi_retry_recovery_count)},
		 {"MNC_SPI_PROTOCOL_ERRORS",
		  std::to_string(health.spi_protocol_error_count)},
		 {"MNC_SPI_REGISTER",
		  std::to_string(health.spi_last_failed_register)},
		 {"MNC_SPI_RECEIVED_HEADER",
		  std::to_string(health.spi_last_received_header)}});
	last_spi_retry_recovery_count_ = health.spi_retry_recovery_count;
}

void RpuHealthMonitor::observe_health_transition(
	const msap1_adc_health_payload &health)
{
	if (last_health_flags_ && *last_health_flags_ == health.health_flags &&
	    last_spi_error_ == health.spi_error)
		return;
	const auto reasons = msap1::evaluate_rpu_adc_health_reasons(health);
	const bool healthy = reasons.empty();
	const auto reason_codes = health_reason_codes(reasons);
	const auto reason_messages = health_reason_messages(reasons);
	log_message(health_log,
		healthy ? mnc::logging::Priority::notice
			: mnc::logging::Priority::warning,
		healthy ? "RPU ADC health became healthy"
			: "RPU ADC health became degraded: " + reason_messages,
		healthy ? "rpu_health_healthy" : "rpu_health_degraded",
		{{"MNC_ADC_HEALTH_FLAGS",
		  std::to_string(health.health_flags)},
		 {"MNC_SPI_ERROR", std::to_string(health.spi_error)},
		 {"MNC_SPI_PROTOCOL_ERRORS",
		  std::to_string(health.spi_protocol_error_count)},
		 {"MNC_SPI_RETRY_RECOVERIES",
		  std::to_string(health.spi_retry_recovery_count)},
		 {"MNC_SPI_REGISTER",
		  std::to_string(health.spi_last_failed_register)},
		 {"MNC_SPI_RECEIVED_HEADER",
		  std::to_string(health.spi_last_received_header)},
		 {"MNC_HEALTH_REASONS", reason_codes},
		 {"MNC_CONFIGURATION_GENERATION",
		  std::to_string(health.meter_generation)}});
	last_health_flags_ = health.health_flags;
	last_spi_error_ = health.spi_error;
}

} // namespace msap1::acquisition::daemon
