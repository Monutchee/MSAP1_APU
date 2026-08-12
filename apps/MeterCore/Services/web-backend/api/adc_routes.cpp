/**
 * @file adc_routes.cpp
 * @brief ADC control endpoints: input source selection, waveform simulator
 *        configuration, and capture start/stop.
 */

#include "health_dto.hpp"
#include "response.hpp"
#include "routes.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <glaze/glaze.hpp>

namespace msap1::web::api {

namespace {

/** Body of GET/PUT/DELETE /api/v1/adc/capture. */
struct AdcCaptureDto {
	bool active;
};

/** Body of GET/PUT /api/v1/adc/source. */
struct AdcSourceDto {
	std::string source = "physical";
	std::uint32_t configuration_generation = 0;
	bool active = false;
	bool healthy = false;
};

/** One simulator channel in GET/PUT /api/v1/adc/simulator. */
struct AdcSimulatorChannelDto {
	std::uint32_t channel = 0;
	double rms = 0.0;
	double phase_degrees = 0.0;
};

/** Body of GET/PUT /api/v1/adc/simulator. */
struct AdcSimulatorDto {
	double frequency_hz = 60.0;
	std::array<AdcSimulatorChannelDto, 8> channels{};
	std::string active_source = "physical";
	std::uint32_t configuration_generation = 0;
	std::uint32_t active_generation = 0;
	std::uint32_t generated_frames = 0;
	std::uint32_t saturation_count = 0;
	std::uint32_t missed_sample_count = 0;
	bool healthy = false;
};

/** @throws std::invalid_argument for anything but physical/simulator. */
std::uint32_t adc_source_value(std::string_view source)
{
	if (source == "physical")
		return MSAP1_ADC_SOURCE_PHYSICAL;
	if (source == "simulator")
		return MSAP1_ADC_SOURCE_SIMULATOR;
	throw std::invalid_argument(
		"ADC source must be physical or simulator");
}

AdcSourceDto adc_source(const msap1::AdcSourceResponse &response)
{
	const bool simulator =
		response.adc_source == MSAP1_ADC_SOURCE_SIMULATOR;
	const bool source_healthy = simulator
		? (response.health_flags &
		   MSAP1_ADC_HEALTH_SIMULATOR_HEALTHY) != 0u
		: (response.health_flags &
		   MSAP1_ADC_HEALTH_PHYSICAL_DIAGNOSTICS) != 0u;
	return {adc_source_name(response.adc_source),
		response.configuration_generation,
		response.running,
		source_healthy};
}

AdcSimulatorDto adc_simulator(const msap1::SimulatorResponse &response)
{
	AdcSimulatorDto result;
	result.frequency_hz =
		static_cast<double>(response.simulator.frequency_millihz) /
		1000.0;
	for (std::size_t index = 0; index < result.channels.size(); ++index) {
		result.channels[index] = {
			static_cast<std::uint32_t>(index),
			response.simulator.channels[index].rms,
			response.simulator.channels[index].phase_degrees,
		};
	}
	result.active_source = adc_source_name(response.adc_source);
	result.configuration_generation = response.configuration_generation;
	result.active_generation = response.simulator_active_generation;
	result.generated_frames = response.simulator_frame_count;
	result.saturation_count = response.simulator_saturation_count;
	result.missed_sample_count = response.simulator_missed_sample_count;
	result.healthy = (response.health_flags &
			  MSAP1_ADC_HEALTH_SIMULATOR_HEALTHY) != 0u;
	return result;
}

/**
 * Validate a requested simulator configuration and convert it to the wire
 * representation.
 *
 * @throws std::invalid_argument for a non-finite frequency/RMS/phase, an
 *         out-of-range frequency, or channel indices that do not cover CH0
 *         through CH7 exactly once.
 */
msap1::SimulatorIpcConfiguration adc_simulator_ipc(
	const AdcSimulatorDto &configuration)
{
	if (!std::isfinite(configuration.frequency_hz) ||
	    configuration.frequency_hz < 0.001 ||
	    configuration.frequency_hz > 1000.0)
		throw std::invalid_argument(
			"simulator frequency must be between 0.001 and 1000 Hz");

	msap1::SimulatorIpcConfiguration result;
	result.frequency_millihz = static_cast<std::uint32_t>(
		std::llround(configuration.frequency_hz * 1000.0));
	std::array<bool, 8> seen{};
	for (const auto &channel : configuration.channels) {
		if (channel.channel >= result.channels.size() ||
		    seen[channel.channel])
			throw std::invalid_argument(
				"simulator channel indices must contain CH0 through CH7 once");
		if (!std::isfinite(channel.rms) || channel.rms < 0.0 ||
		    !std::isfinite(channel.phase_degrees))
			throw std::invalid_argument(
				"simulator RMS and phase values must be finite");
		seen[channel.channel] = true;
		result.channels[channel.channel] = {
			channel.rms, channel.phase_degrees};
	}
	return result;
}

msap1::SimulatorConfig adc_simulator_settings(
	const AdcSimulatorDto &configuration)
{
	/* Reuse the wire conversion as the single validation authority, then
	 * preserve the human-facing values in the persistent document. */
	(void)adc_simulator_ipc(configuration);
	msap1::SimulatorConfig result;
	result.frequency_hz = configuration.frequency_hz;
	result.channels.clear();
	result.channels.reserve(configuration.channels.size());
	for (const auto &channel : configuration.channels)
		result.channels.push_back(
			{channel.channel, channel.rms, channel.phase_degrees});
	return result;
}

/**
 * Shared implementation for the three /api/v1/adc/capture methods.
 *
 * @param start_capture std::nullopt reports the current state (GET);
 *        true starts capture (PUT); false stops it (DELETE).
 */
webengine::Response capture_command(AppContext &app,
				    std::optional<bool> start_capture)
{
	const bool query = !start_capture.has_value();
	const auto correlation = request_id();
	const bool starting = start_capture.value_or(false);
	if (!query)
		log_api_event(mnc::logging::Priority::info,
			starting ? "ADC capture start requested"
				 : "ADC capture stop requested",
			starting ? "capture_start_requested"
				 : "capture_stop_requested",
			{{"MNC_REQUEST_ID", correlation}});
	try {
		bool running = false;
		if (query) {
			const auto response = app.acquisition.information(3000);
			require_acquisition_ok(response.status);
			running = response.running;
		} else {
			const auto response =
				app.acquisition.set_capture(starting);
			require_acquisition_ok(response.status);
			running = response.running;
		}
		if (!query)
			log_api_event(mnc::logging::Priority::notice,
				starting ? "ADC capture started"
					 : "ADC capture stopped",
				starting ? "capture_start_applied"
					 : "capture_stop_applied",
				{{"MNC_REQUEST_ID", correlation}});
		return json_response(webengine::http::status::ok,
			AdcCaptureDto{running});
	} catch (const std::exception &error) {
		log_api_event(mnc::logging::Priority::error,
			std::string("ADC capture command failed: ") +
				error.what(),
			"capture_command_failed",
			{{"MNC_REQUEST_ID", correlation}});
		return error_response(
			webengine::http::status::service_unavailable,
			error.what());
	}
}

} // namespace

/**
 * @brief GET /api/v1/adc/source (Viewer)
 *
 * Reports which ADC input source (physical front end or simulator) is
 * active, its configuration generation, and whether that source is healthy.
 *
 * @return 200 with the source document, or 503 when the acquisition daemon
 *         is unreachable.
 */
webengine::Response get_adc_source(AppContext &app,
				   const webengine::RequestContext &)
{
	try {
		const auto response = app.acquisition.adc_source();
		require_acquisition_ok(response.status);
		return json_response(webengine::http::status::ok,
			adc_source(response));
	} catch (const std::exception &error) {
		log_api_failure("/api/v1/adc/source", error);
		return error_response(
			webengine::http::status::service_unavailable,
			error.what());
	}
}

/**
 * @brief PUT /api/v1/adc/source (Admin)
 *
 * Switches between the physical ADC and the simulator.  The change is
 * persisted through the settings authority, which hot-applies it via the
 * daemon's coordinated stop/configure/restart transaction, then the active
 * source is read back.
 *
 * @return 200 with the applied source, 400 for invalid JSON or an unknown
 *         source name, or 503 when apply/readback fails.
 */
webengine::Response put_adc_source(AppContext &app,
				   const webengine::RequestContext &context)
{
	const auto correlation = request_id();
	try {
		AdcSourceDto configuration;
		if (const auto error = glz::read_json(
			    configuration, context.request.body()))
			return error_response(
				webengine::http::status::bad_request,
				"invalid ADC source JSON");
		(void)adc_source_value(configuration.source);
		(void)app.settings.update_and_save(
			[&](auto &settings) {
				settings.adc.source = configuration.source;
			});
		const auto response = app.acquisition.adc_source();
		require_acquisition_ok(response.status);
		log_api_event(mnc::logging::Priority::notice,
			"ADC source changed to " + configuration.source,
			"adc_source_changed",
			{{"MNC_REQUEST_ID", correlation},
			 {"MNC_ADC_SOURCE", configuration.source},
			 {"MNC_CONFIGURATION_GENERATION",
			  std::to_string(response.configuration_generation)}});
		return json_response(webengine::http::status::ok,
			adc_source(response));
	} catch (const std::invalid_argument &error) {
		return error_response(webengine::http::status::bad_request,
			error.what());
	} catch (const std::exception &error) {
		log_api_failure("/api/v1/adc/source", error);
		return error_response(
			webengine::http::status::service_unavailable,
			error.what());
	}
}

/**
 * @brief GET /api/v1/adc/simulator (Viewer)
 *
 * Reports the simulator configuration (frequency and per-channel RMS and
 * phase) together with its runtime counters and health.
 *
 * @return 200 with the simulator document, or 503 when the acquisition
 *         daemon is unreachable.
 */
webengine::Response get_adc_simulator(AppContext &app,
				      const webengine::RequestContext &)
{
	try {
		const auto response = app.acquisition.simulator_configuration();
		require_acquisition_ok(response.status);
		return json_response(webengine::http::status::ok,
			adc_simulator(response));
	} catch (const std::exception &error) {
		log_api_failure("/api/v1/adc/simulator", error);
		return error_response(
			webengine::http::status::service_unavailable,
			error.what());
	}
}

/**
 * @brief PUT /api/v1/adc/simulator (Admin)
 *
 * Validates and persists a simulator configuration through the settings
 * authority (which hot-applies it), then returns the read-back state.
 *
 * @return 200 with the applied configuration, 400 for invalid JSON or
 *         out-of-range values, or 503 when apply/readback fails.
 */
webengine::Response put_adc_simulator(AppContext &app,
				      const webengine::RequestContext &context)
{
	const auto correlation = request_id();
	try {
		AdcSimulatorDto configuration;
		if (const auto error = glz::read_json(
			    configuration, context.request.body()))
			return error_response(
				webengine::http::status::bad_request,
				"invalid ADC simulator JSON");
		(void)app.settings.update_and_save(
			[&](auto &settings) {
				settings.adc.simulator =
					adc_simulator_settings(configuration);
			});
		const auto response = app.acquisition.simulator_configuration();
		require_acquisition_ok(response.status);
		log_api_event(mnc::logging::Priority::notice,
			"ADC simulator configuration applied",
			"adc_simulator_configuration_applied",
			{{"MNC_REQUEST_ID", correlation},
			 {"MNC_CONFIGURATION_GENERATION",
			  std::to_string(response.configuration_generation)}});
		return json_response(webengine::http::status::ok,
			adc_simulator(response));
	} catch (const std::invalid_argument &error) {
		return error_response(webengine::http::status::bad_request,
			error.what());
	} catch (const std::exception &error) {
		log_api_failure("/api/v1/adc/simulator", error);
		return error_response(
			webengine::http::status::service_unavailable,
			error.what());
	}
}

/**
 * @brief GET /api/v1/adc/capture (Viewer)
 *
 * Reports whether the acquisition capture pipeline is currently running.
 *
 * @return 200 with {"active": bool}, or 503 when the daemon is unreachable.
 */
webengine::Response get_adc_capture(AppContext &app,
				    const webengine::RequestContext &)
{
	return capture_command(app, std::nullopt);
}

/**
 * @brief PUT /api/v1/adc/capture (Admin)
 *
 * Starts the ADC capture pipeline (DMA arm, R5 configuration commit, and
 * capture START are coordinated by the acquisition daemon).
 *
 * @return 200 with the resulting capture state, or 503 on failure.
 */
webengine::Response put_adc_capture(AppContext &app,
				    const webengine::RequestContext &)
{
	return capture_command(app, true);
}

/**
 * @brief DELETE /api/v1/adc/capture (Admin)
 *
 * Stops the ADC capture pipeline.
 *
 * @return 200 with the resulting capture state, or 503 on failure.
 */
webengine::Response delete_adc_capture(AppContext &app,
				       const webengine::RequestContext &)
{
	return capture_command(app, false);
}

} // namespace msap1::web::api
