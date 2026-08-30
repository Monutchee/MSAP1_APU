#include "pipeline/capture_coordinator.hpp"

#include "config/runtime_config.hpp"
#include "ipc/command_handlers.hpp"
#include "msap1/acquisition/rpu/protocol.hpp"
#include "support/logs.hpp"
#include "support/utc_clock.hpp"

#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <exception>
#include <limits>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#include <poll.h>

namespace msap1::acquisition::daemon {

using namespace std::chrono_literals;

msap1::PreparedMeterConfiguration prepare_product_configuration(
	const msap1::settings::ProductSettings &settings)
{
	auto result = msap1::prepare_meter_configuration(
		msap1::settings::to_meter_configuration(settings),
		settings.metering.sample_rate_hz);
	result.m18_wire = msap1::settings::to_m18_configuration(
		settings, result.wire.generation);
	msap1::coordinate_configuration_generation(
		result.wire, result.m18_wire);
	return result;
}

msap1_demand_config_payload demand_configuration(
	const msap1::settings::DemandSettings &settings)
{
	return {
		settings.method == "fixed_block" ? MSAP1_DEMAND_METHOD_FIXED_BLOCK
			: MSAP1_DEMAND_METHOD_SLIDING,
		settings.window_seconds,
		settings.method == "fixed_block" ? 600U : 3U,
	};
}

std::string event_label(PowerQualityLifecycleType type)
{
	switch (type) {
	case PowerQualityLifecycleType::voltage_sag: return "voltage sag";
	case PowerQualityLifecycleType::voltage_swell: return "voltage swell";
	case PowerQualityLifecycleType::voltage_interruption:
		return "voltage interruption";
	case PowerQualityLifecycleType::rapid_voltage_change:
		return "rapid voltage change";
	case PowerQualityLifecycleType::voltage_unbalance:
		return "voltage unbalance";
	case PowerQualityLifecycleType::current_sag: return "current sag";
	case PowerQualityLifecycleType::current_swell: return "current swell";
	case PowerQualityLifecycleType::current_unbalance:
		return "current unbalance";
	case PowerQualityLifecycleType::transient_voltage:
		return "transient voltage";
	}
	return "power-quality event";
}

MncwfTimeQuality waveform_time_quality(meter::TimeQuality quality)
{
	switch (quality) {
	case meter::TimeQuality::Synchronized: return MncwfTimeQuality::locked;
	case meter::TimeQuality::Holdover: return MncwfTimeQuality::holdover;
	case meter::TimeQuality::Unsynchronized:
		return MncwfTimeQuality::unlocked;
	}
	return MncwfTimeQuality::unknown;
}

MncwfV4EventDescriptor waveform_event_descriptor(
	const PowerQualityEventLifecycleSnapshot &snapshot)
{
	MncwfV4EventDescriptor result{};
	result.event_uuid = mncwf_stable_event_uuid(
		snapshot.id.session, snapshot.id.counter);
	result.taxonomy = snapshot.iec_classification
		? MncwfEventTaxonomy::iec_61000_4_30
		: MncwfEventTaxonomy::product_alarm;
	result.event_type = static_cast<std::uint16_t>(snapshot.type) + 1u;
	result.lifecycle = static_cast<MncwfEventLifecycle>(
		static_cast<std::uint16_t>(snapshot.lifecycle) + 1u);
	result.time_quality = waveform_time_quality(snapshot.time_quality);
	result.flags = mncwf_event_start_valid | mncwf_event_current_valid |
		mncwf_event_trigger_valid | mncwf_event_settings_snapshot_valid;
	if (snapshot.terminal())
		result.flags |= mncwf_event_end_valid;
	if (snapshot.start_utc_nanoseconds != 0u &&
	    snapshot.last_utc_nanoseconds != 0u)
		result.flags |= mncwf_event_utc_valid;
	if (snapshot.discontinuities != 0u)
		result.flags |= mncwf_event_contaminated |
			mncwf_event_discontinuous;
	result.phase_mask = snapshot.phase_mask;
	result.quantity = static_cast<std::uint8_t>(snapshot.type) >=
		static_cast<std::uint8_t>(PowerQualityLifecycleType::current_sag) &&
		static_cast<std::uint8_t>(snapshot.type) <=
		static_cast<std::uint8_t>(PowerQualityLifecycleType::current_unbalance)
		? MncwfQuantity::current : MncwfQuantity::voltage;
	result.si_unit = result.quantity == MncwfQuantity::current
		? MncwfSiUnit::ampere : MncwfSiUnit::volt;
	result.trigger_source = snapshot.trigger_source;
	result.configuration_generation = snapshot.configuration_generation;
	result.start_sequence = snapshot.first_sample;
	result.current_sequence = snapshot.last_sample;
	result.end_sequence = snapshot.terminal() ? snapshot.last_sample : 0u;
	result.trigger_sequence = snapshot.trigger_sample;
	result.start_utc_nanoseconds = snapshot.start_utc_nanoseconds;
	result.current_utc_nanoseconds = snapshot.last_utc_nanoseconds;
	result.end_utc_nanoseconds = snapshot.terminal()
		? snapshot.last_utc_nanoseconds : 0u;
	result.trigger_utc_nanoseconds = snapshot.start_utc_nanoseconds;
	result.reference_micro_units = snapshot.reference_micro_units;
	result.threshold_micro_units = static_cast<std::int64_t>(
		(static_cast<std::uint64_t>(snapshot.reference_micro_units) *
			snapshot.threshold_e4 + 5000u) / 10000u);
	result.hysteresis_micro_units = static_cast<std::int64_t>(
		(static_cast<std::uint64_t>(snapshot.reference_micro_units) *
			snapshot.hysteresis_e4 + 5000u) / 10000u);
	const bool minimum_event =
		snapshot.type == PowerQualityLifecycleType::voltage_sag ||
		snapshot.type == PowerQualityLifecycleType::voltage_interruption ||
		snapshot.type == PowerQualityLifecycleType::current_sag;
	for (std::size_t phase = 0; phase < result.extrema_micro_units.size();
	     ++phase)
		result.extrema_micro_units[phase] = minimum_event
			? snapshot.minimum_micro_units[phase]
			: snapshot.maximum_micro_units[phase];
	result.duration_samples = snapshot.duration_samples;
	result.update_count = snapshot.update_count;
	result.status = snapshot.status;
	result.taxonomy_name = snapshot.iec_classification
		? "IEC 61000-4-30 voltage event" : "MSAP1 product alarm";
	result.label = event_label(snapshot.type);
	std::ostringstream settings;
	settings << "{\"configuration_generation\":"
		 << snapshot.configuration_generation
		 << ",\"profile_generation\":" << snapshot.profile_generation
		 << ",\"reference_micro_units\":"
		 << snapshot.reference_micro_units
		 << ",\"threshold_e4\":" << snapshot.threshold_e4
		 << ",\"hysteresis_e4\":" << snapshot.hysteresis_e4
		 << ",\"phase_mask\":" << static_cast<unsigned>(snapshot.phase_mask)
		 << ",\"per_phase\":" << (snapshot.per_phase ? "true" : "false")
		 << ",\"waveform\":{\"pretrigger_ms\":"
		 << snapshot.waveform_pretrigger_ms
		 << ",\"posttrigger_ms\":" << snapshot.waveform_posttrigger_ms
		 << ",\"decimation\":" << snapshot.waveform_decimation
		 << "},\"settings_digest\":\"" << std::hex << std::setfill('0');
	for (const auto word : snapshot.settings_digest)
		settings << std::setw(8) << word;
	settings << "\"}";
	result.settings_snapshot_json = settings.str();
	return result;
}

CaptureCoordinator::CaptureCoordinator(const Options &options)
	: options_(options),
	  product_settings_(load_runtime_settings()),
	  configuration_(prepare_product_configuration(product_settings_)),
	  meter_(options.meter_device),
	  waveform_(options.waveform_device, options.waveform_directory,
		    waveform_capture_context(product_settings_, configuration_)),
	  rpu_(options.service, options.rpmsg_device),
	  meter_stream_(),
	  ingest_(meter_, configuration_, timebase_, meter_stream_,
		  [this](const msap1::PowerQualityEventLifecycleSnapshot &event) {
			  if (!event.waveform_enabled)
				  return;
			  const auto session = waveform_.track_power_quality_event(
				  {event.id.session, event.id.counter},
				  static_cast<msap1::WaveformEventLifecycle>(
					  event.lifecycle),
				  event.trigger_sample, event.last_sample,
				  event.waveform_pretrigger_ms,
				  event.waveform_posttrigger_ms,
				  event.waveform_decimation,
				  waveform_event_descriptor(event));
			  event_waveform_linker_.enqueue(event.id,
				  session.capture_uuid);
		  }),
	  snapshot_provider_(ingest_.meter_data()),
	  meter_data_provider_(snapshot_provider_, meter_stream_),
	  health_(rpu_),
	  aggregation_health_(options.aggregation_service,
			      options.aggregation_rpmsg_device),
	  ipc_(options.socket_path)
{
	register_acquisition_commands(registry_, *this);
	ipc_.start();
}

CaptureCoordinator::~CaptureCoordinator()
{
	stop();
	ipc_.shutdown();
}

void CaptureCoordinator::run()
{
	start();
	try {
		(void)aggregation_health_.configure_demand(
			demand_configuration(product_settings_.metering.demand));
	} catch (const std::exception &error) {
		/* R5C1 may appear after R5C0 during boot. The monitor retains the
		 * desired profile and retries it on endpoint rediscovery. */
		log_message(aggregation_log, mnc::logging::Priority::warning,
			"R5C1 demand profile will be retried: " +
				std::string(error.what()),
			"demand_configuration_deferred");
	}
	log_message(lifecycle_log, mnc::logging::Priority::notice,
		"meter acquisition started: " + std::string(meter_.name()) +
			", configuration generation " +
			std::to_string(configuration_.wire.generation),
		"service_started",
		{{"MNC_CONFIGURATION_GENERATION",
		  std::to_string(configuration_.wire.generation)},
		 {"MNC_DEVICE", std::string(meter_.name())}});
	while (!stop_requested_) {
		service_poll_events();
		/* R5C1 is an independent optional endpoint during shadow bring-up;
		 * its audit runs even while capture is stopped and can never throw
		 * into this acquisition loop. */
		aggregation_health_.periodic_audit();
		if (running_) {
			health_.periodic_audit();
			refresh_time_sync();
		}
	}
	log_message(lifecycle_log, mnc::logging::Priority::notice,
		"meter acquisition service is stopping", "service_stopping");
}

void CaptureCoordinator::service_poll_events()
{
	pollfd descriptors[3] = {
		{meter_.native_handle(), POLLIN, 0},
		{waveform_.fd(), POLLIN, 0},
		{ipc_.event_fd(), POLLIN, 0},
	};
	const int result = ::poll(descriptors, 3, 250);
	if (result < 0) {
		if (errno == EINTR)
			return;
		throw_errno("poll acquisition devices");
	}
	if ((descriptors[0].revents & POLLIN) != 0)
		ingest_.read_available();
	if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
		ingest_.note_dma_failure();
		log_message(dma_log, mnc::logging::Priority::error,
			"meter DMA device disconnected", "dma_disconnected");
		throw std::runtime_error("meter DMA device disconnected");
	}
	if ((descriptors[1].revents & POLLIN) != 0) {
		try {
			waveform_.read_available();
		} catch (const std::exception &error) {
			log_message(waveform_log,
				mnc::logging::Priority::error,
				"waveform DMA read failed: " +
					std::string(error.what()),
				"waveform_read_failed");
		}
	}
	if ((descriptors[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
		throw std::runtime_error("waveform DMA device disconnected");
	if ((descriptors[2].revents & POLLIN) != 0)
		ipc_.drain(registry_);
}

void CaptureCoordinator::refresh_time_sync(bool force)
{
	static constexpr auto time_sync_interval = 10s;
	const auto now = Clock::now();
	if (!force && last_time_sync_ &&
	    now - *last_time_sync_ < time_sync_interval)
		return;
	/* Rate-limit even on failure: a broken correlation source must not
	 * turn the poll loop into an ioctl storm, and a missing refresh is
	 * exactly what the Holdover state exists to report. */
	last_time_sync_ = now;
	/*
	 * The waveform device stays open for the whole capture lifetime, so
	 * the correlation latch is readable here without any capture session.
	 * Its frame_sequence field carries the PL 64-bit conversion-domain
	 * sample counter (PL change, same ioctl ABI).
	 */
	const auto sync = waveform_.time_sync();
	if (!sync)
		return;
	/*
	 * The correlation latch sits before the PL elasticity FIFO, so the
	 * latched counter may lead the conversion stream by up to 16 frames;
	 * add that bound (in nanoseconds at the active rate) to the
	 * CLOCK_REALTIME bracket width. The kernel's own clock error
	 * estimate joins the same combined bound, and its discipline flag
	 * decides whether this sync may claim Synchronized at all.
	 */
	const auto sample_rate = configuration_.wire.sample_rate_hz;
	const std::uint64_t elasticity_ns = sample_rate == 0u
		? 0u
		: 16ull * 1'000'000'000ull / sample_rate;
	const auto clock_status = read_utc_clock_status();
	std::uint16_t leap_flags = 0u;
	if (clock_status.positive_leap_pending)
		leap_flags |= mncwf_time_positive_leap_pending;
	if (clock_status.negative_leap_pending)
		leap_flags |= mncwf_time_negative_leap_pending;
	waveform_.set_time_context(MncwfClockSource::system,
		clock_status.synchronized ? MncwfTimeQuality::locked
			: MncwfTimeQuality::unlocked,
		leap_flags);
	timebase_.record_sync(
		{.sample_counter = sync->sample_counter,
		 .utc_ns = static_cast<std::int64_t>(sync->realtime_nanoseconds),
		 .uncertainty_ns = sync->bracket_nanoseconds + elasticity_ns +
			 clock_status.estimated_uncertainty_ns,
		 /* Bind the sync to the ACTIVE configuration so the timebase
		  * can refuse to extrapolate across a rate change. */
		 .sample_rate_hz = sample_rate,
		 .configuration_generation = configuration_.wire.generation,
			 .utc_synchronized = clock_status.synchronized},
		now);

	/*
	 * M13 maps the NEXT wall-clock ten-minute boundary into the same
	 * free-running sample-counter domain as the aggregation engine. The PL
	 * then advances the target by sample_rate * 600 after every close, so
	 * Linux only needs to seed each new capture/configuration epoch.
	 *
	 * Use a ceiling division: closing one sample early would assign a block
	 * to the wrong UTC interval, while closing on the first eligible basic
	 * block at/after the target is explicitly represented by the record's
	 * overshoot field.
	 */
	if (!ten_minute_boundary_programmed_ && sample_rate != 0u &&
	    clock_status.synchronized) {
		static constexpr std::uint64_t interval_ns =
			600ull * 1'000'000'000ull;
		const auto remainder = sync->realtime_nanoseconds % interval_ns;
		const auto delta_ns = interval_ns - remainder;
		if (delta_ns >
		    std::numeric_limits<std::uint64_t>::max() / sample_rate) {
			log_message(config_log, mnc::logging::Priority::error,
				"ten-minute UTC boundary mapping overflowed",
				"ten_minute_boundary_overflow");
			return;
		}
		const auto scaled = delta_ns * sample_rate;
		const auto frames_until_boundary =
			(scaled + 1'000'000'000ull - 1ull) /
			1'000'000'000ull;
		if (sync->sample_counter >
		    std::numeric_limits<std::uint64_t>::max() -
			    frames_until_boundary) {
			log_message(config_log, mnc::logging::Priority::error,
				"ten-minute PL sample target overflowed",
				"ten_minute_boundary_overflow");
			return;
		}
		const auto target =
			sync->sample_counter + frames_until_boundary;
		try {
			waveform_.program_ten_minute_boundary(target, true);
			ten_minute_boundary_programmed_ = true;
			log_message(config_log, mnc::logging::Priority::notice,
				"programmed next UTC ten-minute aggregation boundary",
				"ten_minute_boundary_programmed",
				{{"MNC_TARGET_SAMPLE_INDEX",
				  std::to_string(target)},
				 {"MNC_SAMPLE_RATE_HZ",
				  std::to_string(sample_rate)}});
		} catch (const std::exception &error) {
			/* Retain the synchronized measurement timebase and retry this
			 * independent control write at the next cadence. */
			log_message(config_log, mnc::logging::Priority::error,
				"failed to program ten-minute boundary: " +
					std::string(error.what()),
				"ten_minute_boundary_program_failed");
		}
	}
}

void CaptureCoordinator::configure_meter()
{
	log_message(config_log, mnc::logging::Priority::info,
		"applying coordinated ADC and PL meter configuration",
		"configuration_apply_started",
		{{"MNC_CONFIGURATION_GENERATION",
		  std::to_string(configuration_.wire.generation)},
		 {"MNC_SAMPLE_RATE_HZ",
		  std::to_string(configuration_.wire.sample_rate_hz)}});
	const auto r5c1_ack =
		aggregation_health_.configure_m18(configuration_.m18_wire);
	if (r5c1_ack.generation != configuration_.wire.generation)
		throw std::runtime_error(
			"R5C1 M18 configuration generation does not match");
	const auto r5c0_m18 = msap1::decode_m18_config_ack(
		rpu_.transact(MSAP1_RPU_MSG_M18_CONFIG_SET,
			&configuration_.m18_wire,
			sizeof(configuration_.m18_wire), 1000ms));
	if (r5c0_m18.generation != configuration_.wire.generation)
		throw std::runtime_error(
			"R5C0 M18 configuration generation does not match");
	const auto response = rpu_.transact(MSAP1_RPU_MSG_METER_CONFIG_SET,
		&configuration_.wire, sizeof(configuration_.wire), 1000ms);
	const auto acknowledgement = msap1::decode_meter_config_ack(response);
	if (acknowledgement.generation != configuration_.wire.generation ||
	    acknowledgement.conversion_active_generation !=
		configuration_.wire.generation ||
	    acknowledgement.processing_active_generation !=
		configuration_.wire.generation ||
	    acknowledgement.adc_source != configuration_.wire.adc_source ||
	    (configuration_.wire.adc_source == MSAP1_ADC_SOURCE_SIMULATOR &&
	     acknowledgement.simulator_active_generation !=
		     configuration_.wire.generation) ||
	    (acknowledgement.conversion_status & 1u) == 0u ||
	    (acknowledgement.processing_status & 1u) == 0u)
		throw std::runtime_error(
			"RPU meter configuration readback does not match");
	log_message(config_log, mnc::logging::Priority::notice,
		"coordinated ADC and PL meter configuration applied",
		"configuration_applied",
		{{"MNC_CONFIGURATION_GENERATION",
		  std::to_string(configuration_.wire.generation)},
		 {"MNC_SAMPLE_RATE_HZ",
		  std::to_string(configuration_.wire.sample_rate_hz)}});
}

void CaptureCoordinator::start(bool apply_configuration)
{
	if (running_)
		return;
	/*
	 * Every deliberate DMA/capture restart starts a new continuity epoch.
	 * Source selection and other coordinated configurations may reset or
	 * advance PL record sequences while DMA is stopped; that boundary is
	 * not packet loss and must not increment the health gap counter.
	 */
	ingest_.begin_epoch();
	ten_minute_boundary_programmed_ = false;
	try {
		/*
		 * Both DMA consumers must own their S2MM channels before the
		 * RPU enables capture. This prevents losing the first waveform
		 * history block after a restart.
		 */
		waveform_.start();
		meter_.start();
		log_message(dma_log, mnc::logging::Priority::info,
			"meter DMA device opened: " + std::string(meter_.name()),
			"dma_opened",
			{{"MNC_DEVICE", std::string(meter_.name())}});
		// PGA and coefficient changes are a coordinated ADC/PL
		// transaction and may only occur with capture stopped. STOP is
		// idempotent, so this also recovers cleanly after a daemon crash.
		const auto stop_response =
			rpu_.transact(MSAP1_RPU_MSG_ADC_CAPTURE_STOP, nullptr,
				      0, 1000ms);
		if (stop_response.header.type != MSAP1_RPU_MSG_ACK ||
		    !stop_response.payload.empty())
			throw std::runtime_error(
				"unexpected RPU capture-stop response");
		if (apply_configuration)
			configure_meter();
		const auto response =
			rpu_.transact(MSAP1_RPU_MSG_ADC_CAPTURE_START, nullptr,
				      0, 1000ms);
		if (response.header.type != MSAP1_RPU_MSG_ACK ||
		    !response.payload.empty())
			throw std::runtime_error(
				"unexpected RPU capture-start response");
	} catch (...) {
		meter_.stop();
		log_message(dma_log, mnc::logging::Priority::info,
			"meter DMA device closed: " + std::string(meter_.name()),
			"dma_closed",
			{{"MNC_DEVICE", std::string(meter_.name())}});
		waveform_.stop();
		throw;
	}
	running_ = true;
	health_.on_capture_started();
	/*
	 * start() is the restart step of every configuration apply
	 * transaction, and a sync point is bound to the generation it was
	 * latched under: refresh immediately so the no-UTC window after a
	 * configuration change lasts one latch, not a full 10 s cadence.
	 */
	refresh_time_sync(/*force=*/true);
	log_message(lifecycle_log, mnc::logging::Priority::notice,
		"ADC capture, meter DMA, and waveform DMA started",
		"capture_started",
		{{"MNC_CONFIGURATION_GENERATION",
		  std::to_string(configuration_.wire.generation)}});
}

void CaptureCoordinator::stop() noexcept
{
	if (!running_)
		return;
	try {
		(void)rpu_.transact(MSAP1_RPU_MSG_ADC_CAPTURE_STOP, nullptr, 0,
				    500ms);
	} catch (const std::exception &error) {
		log_message(rpmsg_log, mnc::logging::Priority::warning,
			"RPU capture stop failed: " + std::string(error.what()),
			"capture_stop_failed");
	}
	meter_.stop();
	log_message(dma_log, mnc::logging::Priority::info,
		"meter DMA device closed: " + std::string(meter_.name()),
		"dma_closed", {{"MNC_DEVICE", std::string(meter_.name())}});
	try {
		waveform_.program_ten_minute_boundary(0u, false);
	} catch (const std::exception &error) {
		log_message(config_log, mnc::logging::Priority::warning,
			"failed to invalidate ten-minute boundary: " +
				std::string(error.what()),
			"ten_minute_boundary_invalidate_failed");
	}
	ten_minute_boundary_programmed_ = false;
	waveform_.stop();
	running_ = false;
	health_.on_capture_stopped();
	log_message(lifecycle_log, mnc::logging::Priority::notice,
		"ADC capture, meter DMA, and waveform DMA stopped",
		"capture_stopped");
}

void CaptureCoordinator::apply_complete_configuration(
	msap1::PreparedMeterConfiguration staged, std::string_view event)
{
	const auto previous = configuration_;
	const bool restart = running_;
	if (restart)
		stop();
	configuration_ = std::move(staged);
	ingest_.clear_latest();
	try {
		if (restart)
			start();
		else
			configure_meter();
		log_message(config_log, mnc::logging::Priority::notice,
			"ADC configuration applied: source=" +
				configuration_.source.adc_source,
			event,
			{{"MNC_CONFIGURATION_GENERATION",
			  std::to_string(configuration_.wire.generation)},
			 {"MNC_ADC_SOURCE",
			  configuration_.source.adc_source}});
	} catch (...) {
		if (running_)
			stop();
		configuration_ = previous;
		ingest_.clear_latest();
		try {
			if (restart)
				start();
			else
				configure_meter();
		} catch (const std::exception &rollback_error) {
			log_message(config_log,
				mnc::logging::Priority::critical,
				"ADC configuration rollback failed: " +
					std::string(rollback_error.what()),
				"adc_configuration_rollback_failed");
		}
		throw;
	}
}

void CaptureCoordinator::apply_product_settings(std::string_view json)
{
	auto candidate = msap1::settings::SettingsCodec::decode(json);
	msap1::settings::SettingsValidator::validate(candidate);
	auto staged = prepare_product_configuration(candidate);
	const bool pipeline_changed =
		staged.wire.generation != configuration_.wire.generation;
	const bool demand_changed =
		candidate.metering.demand.method !=
			product_settings_.metering.demand.method ||
		candidate.metering.demand.window_seconds !=
			product_settings_.metering.demand.window_seconds;
	const auto previous_demand =
		demand_configuration(product_settings_.metering.demand);
	try {
		if (demand_changed)
			(void)aggregation_health_.configure_demand(
				demand_configuration(candidate.metering.demand));
		if (pipeline_changed) {
			apply_complete_configuration(std::move(staged),
				"central_settings_applied");
		} else {
			log_message(config_log, mnc::logging::Priority::notice,
				"live service settings refreshed without restarting capture",
				"central_settings_live_applied");
		}
	} catch (...) {
		if (demand_changed) {
			try {
				(void)aggregation_health_.configure_demand(previous_demand);
			} catch (const std::exception &rollback_error) {
				log_message(config_log, mnc::logging::Priority::critical,
					"demand configuration rollback failed: " +
						std::string(rollback_error.what()),
					"demand_configuration_rollback_failed");
			}
		}
		throw;
	}
	auto capture_context = waveform_capture_context(candidate, configuration_);
	product_settings_ = std::move(candidate);
	waveform_.set_context(std::move(capture_context));
}

void CaptureCoordinator::apply_sample_rate(std::uint32_t sample_rate_hz)
{
	if (!msap1::supported_adc_sample_rate(sample_rate_hz))
		throw std::invalid_argument("unsupported ADC sample rate");

	auto staged = msap1::prepare_meter_configuration(
		configuration_.source, sample_rate_hz);
	auto staged_settings = product_settings_;
	staged_settings.metering.sample_rate_hz = sample_rate_hz;
	staged.m18_wire = msap1::settings::to_m18_configuration(
		staged_settings, staged.wire.generation);
	msap1::coordinate_configuration_generation(
		staged.wire, staged.m18_wire);
	const auto previous = configuration_;
	const bool restart = running_;
	if (restart)
		stop();
	configuration_ = std::move(staged);
	ingest_.clear_latest();

	try {
		if (restart) {
			// start() arms DMA before committing the coordinated
			// ADC/PL configuration and requesting capture.
			start();
		} else {
			// A stopped pipeline still applies the operating point
			// now so `mnc adc rate` can diagnose DRDY without first
			// starting DMA and capture.
			configure_meter();
			health_.refresh();
		}
	} catch (...) {
		if (running_)
			stop();
		configuration_ = previous;
		ingest_.clear_latest();
		try {
			if (restart) {
				start();
			} else {
				configure_meter();
				health_.refresh();
			}
		} catch (const std::exception &rollback_error) {
			log_message(config_log,
				mnc::logging::Priority::critical,
				"sample-rate rollback failed: " +
					std::string(rollback_error.what()),
				"sample_rate_rollback_failed");
		}
		throw;
	}
	log_message(config_log, mnc::logging::Priority::notice,
		"temporary ADC sample rate applied: " +
			std::to_string(sample_rate_hz) + " frame/s",
		"sample_rate_applied",
		{{"MNC_SAMPLE_RATE_HZ", std::to_string(sample_rate_hz)},
		 {"MNC_CONFIGURATION_GENERATION",
		  std::to_string(configuration_.wire.generation)}});
	waveform_.set_context(
		waveform_capture_context(staged_settings, configuration_));
}

void CaptureCoordinator::run_adc_diagnostic(std::uint32_t flow)
{
	if (flow != 1u)
		throw std::invalid_argument("unsupported ADC diagnostic flow");

	const bool restart = running_;
	if (restart)
		stop();

	try {
		const msap1_adc_diagnostic_request request{flow};
		last_adc_diagnostic_ = msap1::decode_adc_diagnostic(
			rpu_.transact(MSAP1_RPU_MSG_ADC_DIAGNOSTIC_RUN,
				      &request, sizeof(request), 15000ms));

		if (restart) {
			/*
			 * A successful flow restores the same ADC operating
			 * point itself. Resume DMA/capture without issuing a
			 * second SRC load, otherwise the final diagnostic
			 * snapshot would no longer describe the active state.
			 * If the flow failed, perform the normal full
			 * coordinated configuration as a recovery attempt.
			 */
			const bool diagnostic_succeeded =
				last_adc_diagnostic_.diagnostic_error ==
				MSAP1_ADC_DIAGNOSTIC_ERROR_NONE;
			start(!diagnostic_succeeded);
		}
		if (!restart)
			health_.refresh();
		log_message(config_log, mnc::logging::Priority::notice,
			"ADC diagnostic flow completed",
			"adc_diagnostic_completed",
			{{"MNC_DIAGNOSTIC_FLOW", std::to_string(flow)},
			 {"MNC_DIAGNOSTIC_ERROR",
			  std::to_string(
				  last_adc_diagnostic_.diagnostic_error)}});
	} catch (...) {
		if (restart && !running_) {
			try {
				start();
			} catch (const std::exception &rollback_error) {
				log_message(config_log,
					mnc::logging::Priority::critical,
					"ADC diagnostic recovery failed: " +
						std::string(
							rollback_error.what()),
					"adc_diagnostic_recovery_failed");
			}
		}
		throw;
	}
}

msap1::InfoResponse CaptureCoordinator::info_response()
{
	msap1::InfoResponse response{};
	response.running = running_;
	response.has_meter_record = ingest_.latest_record().has_value();
	response.has_aggregate_record =
		ingest_.latest_aggregate_record().has_value();
	response.health_probe_pending = health_.probe_pending();
	response.sample_rate_hz = configuration_.wire.sample_rate_hz;
	response.configuration_generation = configuration_.wire.generation;
	response.meter_record_age_ms = ingest_.record_age_ms();
	response.aggregate_record_age_ms = ingest_.aggregate_record_age_ms();
	response.rpu_health_age_ms = health_.health_age_ms();
	response.health_probe_failures = health_.probe_failures();
	response.adc_source = configuration_.wire.adc_source;
	response.meter_records = ingest_.meter_records();
	response.dma_bytes = ingest_.dma_bytes();
	response.dma_read_errors = ingest_.dma_read_errors();
	response.invalid_records = ingest_.invalid_records();
	response.lifetime_invalid_records = ingest_.lifetime_invalid_records();
	response.sequence_gaps = ingest_.sequence_gaps();
	/* One sample of the whole kernel accounting, so produced, consumed,
	 * overrun, and callbacks on the wire all describe the same instant. */
	const auto transport = ingest_.transport_status();
	response.transport_produced_blocks = transport.produced_blocks;
	response.transport_consumed_blocks = transport.consumed_blocks;
	response.transport_overrun_blocks = transport.overrun_blocks;
	response.transport_callbacks = transport.callbacks;
	response.transport_ring_blocks = transport.ring_blocks;
	response.time_quality = timebase_.quality(Clock::now());
	/* Provenance of the cached aggregate, captured at ingest — NOT
	 * timebase_.quality() above, which is the clock's state at this
	 * reply, potentially seconds to minutes after the measurement. */
	response.aggregate_time_quality =
		ingest_.latest_aggregate_time_quality();
	response.rpu_health = health_.cached();
	response.has_aggregation_health =
		aggregation_health_.has_cached_health();
	response.aggregation_health_probe_pending =
		aggregation_health_.probe_pending();
	response.aggregation_health_age_ms =
		aggregation_health_.health_age_ms();
	response.aggregation_health_probe_failures =
		aggregation_health_.probe_failures();
	response.aggregation_rpmsg_device = aggregation_health_.device_path();
	if (response.has_aggregation_health)
		response.rpu_aggregation_health = aggregation_health_.cached();
	if (ingest_.latest_record())
		response.latest_record = *ingest_.latest_record();
	if (ingest_.latest_aggregate_record())
		response.latest_aggregate_record =
			*ingest_.latest_aggregate_record();
	return response;
}

msap1::MeterSnapshotResponse CaptureCoordinator::meter_snapshot_response(
	const msap1::MeterSnapshotRequest &request) const
{
	msap1::MeterSnapshotResponse response{};
	response.running = running_;
	if (const auto snapshot = meter_data_provider_.snapshot_provider().latest(
		request.selection)) {
		response.has_snapshot = true;
		response.snapshot = *snapshot;
		/* The SQLite ledger owns lifetime values and reset epochs. Overlay it
		 * at read time so REST/MQTT/Modbus cannot retain pre-reset peaks until
		 * the next (potentially ten-minute) RPU family. */
		try {
			if (request.selection.period ==
			    mnc::meter::MeasurementPeriod::Basic) {
				if (const auto energy = meter_stream_.energy())
					msap1::meter::overlay_authoritative_energy(
						response.snapshot, *energy);
			} else if (request.selection.period ==
				   mnc::meter::MeasurementPeriod::Demand) {
				if (const auto demand = meter_stream_.demand())
					msap1::meter::overlay_authoritative_demand(
						response.snapshot, *demand);
			}
		} catch (const std::exception &) {
			/* Preserve the last acknowledged durable view while meter-stream is
			 * temporarily unavailable; the next request retries the authority. */
		}
	}

	/* Basic-record transport diagnostics are meaningful only for the current
	 * record. They deliberately remain outside the generic provider model. */
	if (request.selection.period != mnc::meter::MeasurementPeriod::Basic ||
	    !ingest_.latest_record())
		return response;
	const auto &record = *ingest_.latest_record();
	response.diagnostics.sample_rate_hz = record.sample_rate_hz();
	response.diagnostics.block_sample_count = record.block_sample_count();
	response.diagnostics.status = record.status();
	response.diagnostics.capture_frames = record.capture_frames();
	response.diagnostics.header_errors = record.header_errors();
	response.diagnostics.fifo_overflows = record.fifo_overflows();
	response.diagnostics.emit_drops = record.emit_drops();
	response.diagnostics.result_drops = record.result_drops();
	for (std::size_t index = 0; index < response.diagnostics.channels.size();
	     ++index) {
		const auto channel = record.channel(index);
		response.diagnostics.channels[index] = {
			channel.mean_micro_units, channel.rms_count};
	}
	const auto frequency = record.frequency();
	response.diagnostics.frequency = {
		frequency.enabled,
		frequency.reference_valid,
		frequency.out_of_range,
		frequency.timed_out,
		frequency.arithmetic_error,
		frequency.period_q16_samples,
		frequency.measurement_sequence,
		frequency.mode,
		frequency.reference_channel,
		frequency.cycles_used,
	};
	/* The typed view is stamped once by RecordIngestor when the record is
	 * accepted. Do not ask the timebase again here: a later transition to
	 * holdover must not rewrite the provenance of this historical block. */
	if (response.snapshot.timing) {
		/* Use the exact snapshot returned above, rather than looking up the
		 * latest view a second time. This keeps diagnostics and typed values
		 * tied to one accepted measurement even if another DMA record arrives
		 * between the two operations. */
		const auto &timing = *response.snapshot.timing;
		const auto record_timing = record.timing();
		const auto quality = [&] {
			switch (timing.quality) {
			case mnc::meter::TimeQuality::Synchronized:
				return msap1::meter::TimeQuality::Synchronized;
			case mnc::meter::TimeQuality::Holdover:
				return msap1::meter::TimeQuality::Holdover;
			case mnc::meter::TimeQuality::Unsynchronized:
				break;
			}
			return msap1::meter::TimeQuality::Unsynchronized;
		}();
		response.diagnostics.timing = msap1::MeterBlockTimingIpc{
			record.sequence(),
			record.first_sample_index(),
			record.block_sample_count(),
			record_timing.cycle_count,
			record_timing.nominal_frequency_hz,
			record_timing.cycle_locked,
			record_timing.free_run_fallback,
			quality,
		};
	}
	return response;
}

msap1::CaptureResponse CaptureCoordinator::capture_response() const
{
	return {msap1::AcquisitionStatus::ok, running_};
}

msap1::FrequencyResponse CaptureCoordinator::frequency_response() const
{
	return {msap1::AcquisitionStatus::ok, configuration_.wire.generation,
		frequency_ipc(configuration_.source.frequency)};
}

msap1::DiagnosticResponse CaptureCoordinator::diagnostic_response() const
{
	msap1::DiagnosticResponse response{};
	response.running = running_;
	response.live_drdy_frequency_hz = health_.cached().drdy_frequency_hz;
	response.diagnostic = last_adc_diagnostic_;
	return response;
}

msap1::SingleCycleResponse CaptureCoordinator::single_cycle_response() const
{
	msap1::SingleCycleResponse response{};
	response.running = running_;
	response.records = ingest_.single_cycle_records();
	const auto &latest = ingest_.latest_single_cycle();
	response.has_snapshot = latest.has_value();
	if (latest)
		response.snapshot = *latest;
	return response;
}

namespace {

/* Project one decoded PQEVT record onto the IPC's plain-scalar shape. */
msap1::PowerQualityIpcSnapshot power_quality_ipc(
	const msap1::PowerQualitySnapshot &snapshot)
{
	const auto &values = snapshot.values;
	msap1::PowerQualityIpcSnapshot wire{};
	wire.sequence = snapshot.sequence;
	wire.configuration_generation = snapshot.configuration_generation;
	wire.sample_rate_hz = snapshot.sample_rate_hz;
	wire.first_sample = snapshot.first_sample;
	wire.last_sample = snapshot.last_sample;
	wire.sample_count = snapshot.sample_count;
	wire.status = snapshot.status;
	wire.valid_mask = snapshot.valid_mask;
	wire.kind = static_cast<std::uint8_t>(values.kind);
	wire.event_type = static_cast<std::uint8_t>(values.event_type);
	wire.affected_phases = values.affected_phases;
	wire.armed = values.armed;
	wire.cycle_locked = values.cycle_locked;
	wire.synthetic_half_cycle = values.synthetic_half_cycle;
	wire.event_sequence = values.event_sequence;
	wire.duration_samples = values.duration_samples;
	wire.half_cycle_updates = values.half_cycle_updates;
	wire.reference_microvolts = values.reference_micro_volts;
	wire.sag_threshold_e4 = values.sag_threshold_e4;
	wire.swell_threshold_e4 = values.swell_threshold_e4;
	wire.interruption_threshold_e4 = values.interruption_threshold_e4;
	wire.hysteresis_e4 = values.hysteresis_e4;
	const msap1::Reading<msap1::MicroVolts> *voltage[3] = {
		&values.voltage.phase_a, &values.voltage.phase_b,
		&values.voltage.phase_c};
	const msap1::Reading<msap1::MicroVolts> *minimum[3] = {
		&values.voltage_minimum.phase_a, &values.voltage_minimum.phase_b,
		&values.voltage_minimum.phase_c};
	const msap1::Reading<msap1::MicroVolts> *maximum[3] = {
		&values.voltage_maximum.phase_a, &values.voltage_maximum.phase_b,
		&values.voltage_maximum.phase_c};
	const msap1::Reading<msap1::MicroAmperes> *current[3] = {
		&values.current.phase_a, &values.current.phase_b,
		&values.current.phase_c};
	for (std::size_t phase = 0; phase < wire.phases.size(); ++phase) {
		wire.phases[phase] = {
			voltage[phase]->value, minimum[phase]->value,
			maximum[phase]->value, current[phase]->value,
			static_cast<std::uint8_t>(voltage[phase]->quality)};
	}
	return wire;
}

} // namespace

msap1::PowerQualityResponse CaptureCoordinator::power_quality_response() const
{
	msap1::PowerQualityResponse response{};
	response.running = running_;
	response.records = ingest_.pq_event_records();
	response.events = ingest_.pq_events();
	const auto &latest = ingest_.latest_power_quality();
	response.has_latest = latest.has_value();
	if (latest)
		response.latest = power_quality_ipc(*latest);
	const auto &event = ingest_.latest_power_quality_event();
	response.has_event = event.has_value();
	if (event)
		response.event = power_quality_ipc(*event);
	return response;
}

msap1::FlickerResponse CaptureCoordinator::flicker_response() const
{
	msap1::FlickerResponse response{};
	response.running = running_;
	response.records = ingest_.flicker_records();
	response.sequence_gaps = ingest_.flicker_sequence_gaps();
	const auto copy = [&response, this](msap1::FlickerRecordKind kind,
		bool &present, msap1::FlickerSnapshot &target) {
		const auto &source = ingest_.latest_flicker(kind);
		present = source.has_value();
		if (source)
			target = *source;
	};
	copy(msap1::FlickerRecordKind::live, response.has_live, response.live);
	copy(msap1::FlickerRecordKind::pst, response.has_pst, response.pst);
	copy(msap1::FlickerRecordKind::plt, response.has_plt, response.plt);
	return response;
}

msap1::MainsSignalResponse CaptureCoordinator::mains_signal_response() const
{
	msap1::MainsSignalResponse response{};
	response.running = running_;
	response.records = ingest_.mains_signal_records();
	response.sequence_gaps = ingest_.mains_signal_sequence_gaps();
	const auto &latest = ingest_.latest_mains_signal();
	response.has_snapshot = latest.has_value();
	if (latest)
		response.snapshot = *latest;
	return response;
}

msap1::HarmonicResponse CaptureCoordinator::harmonic_response(
	msap1::MeasurementPeriod period) const
{
	if (period != msap1::MeasurementPeriod::Cycles150_180 &&
	    period != msap1::MeasurementPeriod::Min10 &&
	    period != msap1::MeasurementPeriod::Hour2 &&
	    period != msap1::MeasurementPeriod::Basic)
		throw std::invalid_argument("unsupported harmonic period");
	msap1::HarmonicResponse response{};
	response.running = running_;
	response.period = period;
	response.records = ingest_.harmonic_records(period);
	response.families = ingest_.harmonic_families(period);
	response.incomplete_families =
		ingest_.incomplete_harmonic_families(period);
	const auto &latest = ingest_.latest_harmonic_spectrum(period);
	response.has_snapshot = latest.has_value();
	if (latest)
		response.snapshot = *latest;
	return response;
}

msap1::SimulatorEventResponse CaptureCoordinator::simulator_event_response(
	const msap1::SimulatorEventRequest &request)
{
	if (!std::isfinite(request.scale_percent) ||
	    request.scale_percent < 0.0 || request.scale_percent > 400.0)
		throw std::runtime_error(
			"simulator event amplitude must be 0..400 percent");

	msap1_simulator_event_payload wire{};
	wire.action = request.action;
	wire.channel_mask = request.channel_mask;
	/* Percent of nominal to the PL's Q16 multiplier; 100 % must land on
	 * exactly unity, which the PL treats as a bit-exact no-op. */
	wire.scale_q16 = static_cast<std::uint32_t>(
		std::llround(request.scale_percent * 65536.0 / 100.0));
	wire.duration_half_cycles = request.duration_half_cycles;
	wire.period_half_cycles = request.period_half_cycles;
	wire.flags = request.repeat
		? static_cast<std::uint32_t>(MSAP1_SIMULATOR_EVENT_FLAG_REPEAT)
		: 0u;

	const auto reply = rpu_.transact(MSAP1_RPU_MSG_SIMULATOR_EVENT_SET,
		&wire, sizeof(wire), 1000ms);
	const auto acknowledgement = msap1::decode_simulator_event_ack(reply);

	msap1::SimulatorEventResponse response{};
	response.running = running_;
	response.adc_source = configuration_.wire.adc_source;
	response.sequencer_status = acknowledgement.status;
	response.remaining = acknowledgement.remaining;
	response.active_control = acknowledgement.active_control;
	response.active_scale = acknowledgement.active_scale;
	response.active_timing = acknowledgement.active_timing;
	return response;
}

msap1::WaveformResponse CaptureCoordinator::waveform_response()
{
	msap1::WaveformResponse response{};
	response.waveform = waveform_.status();
	for (const auto &session : waveform_.sessions()) {
		const auto capture_uuid = mncwf_uuid_is_zero(session.capture_uuid)
			? std::string{} : mncwf_uuid_string(session.capture_uuid);
		response.sessions.push_back(
			{session.id, session.trigger_sequence,
			 session.first_sequence, session.last_sequence,
			 session.trigger_tai_nanoseconds,
			 session.trigger_realtime_nanoseconds,
			 session.sample_rate_hz, session.event_count,
			 session.trigger_source_mask,
			 session.state, session.decimation,
			 std::string(session.filename.data()),
			 session.continuation_of_session_id,
			 session.master_session_id, capture_uuid});
	}
	response.waveform_directory = options_.waveform_directory;
	return response;
}

msap1::AdcSourceResponse CaptureCoordinator::adc_source_response() const
{
	msap1::AdcSourceResponse response{};
	response.running = running_;
	response.adc_source = configuration_.wire.adc_source;
	response.configuration_generation = configuration_.wire.generation;
	response.health_flags = health_.cached().health_flags;
	return response;
}

msap1::SimulatorResponse CaptureCoordinator::simulator_response() const
{
	msap1::SimulatorResponse response{};
	response.adc_source = configuration_.wire.adc_source;
	response.configuration_generation = configuration_.wire.generation;
	response.health_flags = health_.cached().health_flags;
	response.simulator_active_generation =
		health_.cached().simulator_active_generation;
	response.simulator_frame_count = health_.cached().simulator_frame_count;
	response.simulator_saturation_count =
		health_.cached().simulator_saturation_count;
	response.simulator_missed_sample_count =
		health_.cached().simulator_missed_sample_count;
	response.simulator = simulator_ipc(configuration_.source.simulator);
	return response;
}

} // namespace msap1::acquisition::daemon
