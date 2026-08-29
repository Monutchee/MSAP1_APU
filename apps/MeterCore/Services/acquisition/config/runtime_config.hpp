#pragma once

/**
 * @file runtime_config.hpp
 * @brief Settings-authority loading and wire/metadata translation helpers.
 *
 * These functions translate between the persistent typed settings document
 * and the representations the pipeline needs: the RPMsg wire configuration,
 * waveform file metadata, and IPC response payloads.
 */

#include "msap1/acquisition/ipc/acquisition_commands.hpp"
#include "msap1/meter/meter_config.hpp"
#include "msap1/settings/settings.hpp"
#include "msap1/waveform/waveform_capture.hpp"

#include <array>

namespace msap1::acquisition::daemon {

/**
 * @brief Fetch the active settings document from the settings authority.
 *
 * The daemon never reads configuration files directly; the settings service
 * is the single persistence authority.
 *
 * @throws std::runtime_error when the settings service is unreachable.
 */
msap1::settings::ProductSettings load_runtime_settings();

/**
 * @brief Freeze all capture and per-channel MNCWF-v4 authorities.
 *
 * The result includes configured identity, firmware/build provenance,
 * configuration and sensor digests, calibration state, exact transforms,
 * sensor ratios, ranges, resolution, clipping, and stable channel UUIDs.
 */
msap1::WaveformCaptureContext waveform_capture_context(
	const msap1::settings::ProductSettings &settings,
	const msap1::PreparedMeterConfiguration &configuration);

/**
 * @brief Render the persistent frequency settings as the IPC configuration
 *        reported by FrequencyGetRequest.
 */
msap1::FrequencyIpcConfiguration frequency_ipc(
	const msap1::FrequencyConfig &frequency);

/**
 * @brief Render the persistent simulator settings as the IPC configuration
 *        reported by SimulatorGetRequest.
 */
msap1::SimulatorIpcConfiguration simulator_ipc(
	const msap1::SimulatorConfig &simulator);

} // namespace msap1::acquisition::daemon
