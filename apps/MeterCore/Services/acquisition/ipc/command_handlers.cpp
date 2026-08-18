#include "ipc/command_handlers.hpp"

#include "pipeline/capture_coordinator.hpp"
#include "support/logs.hpp"

#include <string>

namespace msap1::acquisition::daemon {

void register_acquisition_commands(msap1::AcquisitionCommandRegistry &registry,
				   CaptureCoordinator &coordinator)
{
	using msap1::AcquisitionStatus;
	registry.set_error_observer(
		[](std::string_view command, std::string_view what) {
			log_message(lifecycle_log,
				mnc::logging::Priority::error,
				"acquisition command failed: " +
					std::string(what),
				"command_failed",
				{{"MNC_COMMAND", std::string(command)}});
		});
	registry.on<msap1::InfoRequest>(AcquisitionStatus::dma_error,
		[&coordinator](const auto &) {
			return coordinator.info_response();
		});
	registry.on<msap1::MeterSnapshotRequest>(AcquisitionStatus::dma_error,
		[&coordinator](const msap1::MeterSnapshotRequest &request) {
			return coordinator.meter_snapshot_response(request);
		});
	// The public health path returns the daemon cache. Web polling
	// must never trigger a 100-register SPI audit.
	registry.on<msap1::HealthRequest>(AcquisitionStatus::dma_error,
		[&coordinator](const auto &) {
			return coordinator.info_response();
		});
	registry.on<msap1::HealthRefreshRequest>(AcquisitionStatus::rpu_error,
		[&coordinator](const auto &) {
			coordinator.refresh_rpu_health();
			return coordinator.info_response();
		});
	registry.on<msap1::StartRequest>(
		AcquisitionStatus::configuration_error,
		[&coordinator](const auto &) {
			coordinator.start();
			return coordinator.capture_response();
		});
	registry.on<msap1::StopRequest>(AcquisitionStatus::dma_error,
		[&coordinator](const auto &) {
			coordinator.stop();
			return coordinator.capture_response();
		});
	registry.on<msap1::FrequencyGetRequest>(AcquisitionStatus::dma_error,
		[&coordinator](const auto &) {
			return coordinator.frequency_response();
		});
	registry.on<msap1::SampleRateSetRequest>(
		AcquisitionStatus::configuration_error,
		[&coordinator](const msap1::SampleRateSetRequest &request) {
			coordinator.apply_sample_rate(request.sample_rate_hz);
			return coordinator.info_response();
		});
	registry.on<msap1::DiagnosticRunRequest>(
		AcquisitionStatus::configuration_error,
		[&coordinator](const msap1::DiagnosticRunRequest &request) {
			coordinator.run_adc_diagnostic(request.flow);
			return coordinator.diagnostic_response();
		});
	registry.on<msap1::WaveformStatusRequest>(AcquisitionStatus::dma_error,
		[&coordinator](const auto &) {
			return coordinator.waveform_response();
		});
	registry.on<msap1::WaveformListRequest>(AcquisitionStatus::dma_error,
		[&coordinator](const auto &) {
			return coordinator.waveform_response();
		});
	registry.on<msap1::WaveformTriggerRequest>(AcquisitionStatus::dma_error,
		[&coordinator](const msap1::WaveformTriggerRequest &request) {
			const auto &defaults =
				coordinator.product_settings().waveform;
			const auto pretrigger_ms =
				request.pretrigger_ms ==
					msap1::waveform_duration_unspecified
				? defaults.default_pretrigger_ms
				: request.pretrigger_ms;
			const auto posttrigger_ms =
				request.posttrigger_ms ==
					msap1::waveform_duration_unspecified
				? defaults.default_posttrigger_ms
				: request.posttrigger_ms;
			const auto decimation = request.decimation == 0u
				? defaults.default_decimation
				: request.decimation;
			try {
				const auto session =
					coordinator.waveform().trigger(
						pretrigger_ms, posttrigger_ms,
						decimation, request.source);
				log_message(waveform_log,
					mnc::logging::Priority::notice,
					"waveform capture triggered",
					"waveform_triggered",
					{{"MNC_WAVEFORM_SESSION",
					  std::to_string(session.id)},
					 {"MNC_PRETRIGGER_MS",
					  std::to_string(pretrigger_ms)},
					 {"MNC_POSTTRIGGER_MS",
					  std::to_string(posttrigger_ms)}});
			} catch (const std::invalid_argument &error) {
				/*
				 * A window the history buffer cannot hold is
				 * a client mistake, not a transport fault:
				 * report bad_request with the usual waveform
				 * status attached so the caller can compute
				 * the current budget, instead of letting the
				 * registry collapse it into a status-only
				 * dma_error rejection.
				 */
				log_message(waveform_log,
					mnc::logging::Priority::warning,
					std::string("waveform trigger "
						    "rejected: ") +
						error.what(),
					"waveform_trigger_rejected",
					{{"MNC_PRETRIGGER_MS",
					  std::to_string(pretrigger_ms)},
					 {"MNC_POSTTRIGGER_MS",
					  std::to_string(posttrigger_ms)}});
				auto response = coordinator.waveform_response();
				response.status =
					AcquisitionStatus::bad_request;
				return response;
			}
			return coordinator.waveform_response();
		});
	registry.on<msap1::WaveformDeleteRequest>(AcquisitionStatus::dma_error,
		[&coordinator](const msap1::WaveformDeleteRequest &request) {
			coordinator.waveform().erase(request.session_id);
			log_message(waveform_log,
				mnc::logging::Priority::notice,
				"waveform capture deleted", "waveform_deleted",
				{{"MNC_WAVEFORM_SESSION",
				  std::to_string(request.session_id)}});
			return coordinator.waveform_response();
		});
	registry.on<msap1::AdcSourceGetRequest>(AcquisitionStatus::dma_error,
		[&coordinator](const auto &) {
			return coordinator.adc_source_response();
		});
	registry.on<msap1::SimulatorGetRequest>(AcquisitionStatus::dma_error,
		[&coordinator](const auto &) {
			return coordinator.simulator_response();
		});
	registry.on<msap1::SingleCycleRequest>(AcquisitionStatus::dma_error,
		[&coordinator](const auto &) {
			return coordinator.single_cycle_response();
		});
	registry.on<msap1::ConfigurationApplyRequest>(
		AcquisitionStatus::configuration_error,
		[&coordinator](const msap1::ConfigurationApplyRequest &request) {
			coordinator.apply_product_settings(
				request.configuration_json);
			return msap1::ApplyResponse{
				msap1::AcquisitionStatus::ok,
				coordinator.configuration_generation()};
		});
}

} // namespace msap1::acquisition::daemon
