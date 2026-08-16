#include "pipeline/capture_coordinator.hpp"

#include "config/runtime_config.hpp"
#include "ipc/command_handlers.hpp"
#include "msap1/acquisition/rpu/protocol.hpp"
#include "support/logs.hpp"
#include "support/utc_clock.hpp"

#include <chrono>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

#include <poll.h>

namespace msap1::acquisition::daemon {

using namespace std::chrono_literals;

CaptureCoordinator::CaptureCoordinator(const Options &options)
	: options_(options),
	  product_settings_(load_runtime_settings()),
	  configuration_(msap1::prepare_meter_configuration(
		  msap1::settings::to_meter_configuration(product_settings_),
		  product_settings_.metering.sample_rate_hz)),
	  meter_(options.meter_device),
	  waveform_(options.waveform_device, options.waveform_directory,
		    waveform_metadata(configuration_)),
	  rpu_(options.service, options.rpmsg_device),
	  meter_stream_(),
	  ingest_(meter_, configuration_, timebase_, meter_stream_),
	  snapshot_provider_(ingest_.meter_data()),
	  meter_data_provider_(snapshot_provider_, meter_stream_),
	  health_(rpu_),
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
	auto meter_settings =
		msap1::settings::to_meter_configuration(candidate);
	const bool pipeline_changed =
		candidate.metering.sample_rate_hz !=
			configuration_.wire.sample_rate_hz ||
		msap1::encode_meter_configuration(meter_settings, false) !=
			msap1::encode_meter_configuration(
				configuration_.source, false);
	if (pipeline_changed) {
		auto staged = msap1::prepare_meter_configuration(
			std::move(meter_settings),
			candidate.metering.sample_rate_hz);
		apply_complete_configuration(std::move(staged),
			"central_settings_applied");
	} else {
		log_message(config_log, mnc::logging::Priority::notice,
			"live service settings refreshed without restarting capture",
			"central_settings_live_applied");
	}
	product_settings_ = std::move(candidate);
}

void CaptureCoordinator::apply_sample_rate(std::uint32_t sample_rate_hz)
{
	if (!msap1::supported_adc_sample_rate(sample_rate_hz))
		throw std::invalid_argument("unsupported ADC sample rate");

	auto staged = msap1::prepare_meter_configuration(
		configuration_.source, sample_rate_hz);
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
	}

	/* MTR1 transport diagnostics are meaningful only for the current basic
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

msap1::WaveformResponse CaptureCoordinator::waveform_response()
{
	msap1::WaveformResponse response{};
	response.waveform = waveform_.status();
	for (const auto &session : waveform_.sessions())
		response.sessions.push_back(
			{session.id, session.trigger_sequence,
			 session.first_sequence, session.last_sequence,
			 session.trigger_tai_nanoseconds,
			 session.trigger_realtime_nanoseconds,
			 session.sample_rate_hz, session.event_count,
			 session.state, std::string(session.filename.data())});
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
