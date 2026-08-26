#pragma once

#include "msap1/acquisition/ipc/acquisition_ipc.hpp"

#include <cstdint>

namespace msap1::web {

/**
 * Typed application boundary between HTTP controllers and acquisition IPC.
 *
 * Controllers express product operations through this class.  They do not
 * construct MNCI frames, manage Unix sockets, or depend on correlation IDs.
 * Every operation returns the response struct of its acquisition command, so
 * a handler receives exactly the data its endpoint renders.
 */
class AcquisitionGateway final {
public:
	AcquisitionGateway() = default;

	[[nodiscard]] InfoResponse information(int timeout_ms = 1000);
	[[nodiscard]] MeterSnapshotResponse meter_snapshot(
		mnc::meter::MeterSnapshotRequest selection = {},
		int timeout_ms = 1000);
	[[nodiscard]] WaveformResponse waveform_status(int timeout_ms = 1000);
	[[nodiscard]] WaveformResponse trigger_waveform(
		std::uint32_t pretrigger_ms, std::uint32_t posttrigger_ms,
		std::uint32_t decimation, WaveformTriggerSource source,
		int timeout_ms = 3000);
	[[nodiscard]] WaveformResponse delete_waveform(
		std::uint64_t session_id, int timeout_ms = 3000);
	[[nodiscard]] FrequencyResponse frequency_configuration(
		int timeout_ms = 1000);
	[[nodiscard]] AdcSourceResponse adc_source(int timeout_ms = 1000);
	[[nodiscard]] SimulatorResponse simulator_configuration(
		int timeout_ms = 1000);
	[[nodiscard]] SingleCycleResponse single_cycle(int timeout_ms = 1000);
	[[nodiscard]] PowerQualityResponse power_quality(int timeout_ms = 1000);
	[[nodiscard]] HarmonicResponse harmonics(int timeout_ms = 1000);
	[[nodiscard]] SimulatorEventResponse simulator_event(
		const SimulatorEventRequest &request, int timeout_ms = 3000);
	[[nodiscard]] CaptureResponse set_capture(
		bool enabled, int timeout_ms = 3000);

private:
	AcquisitionClient client_;
};

} // namespace msap1::web
