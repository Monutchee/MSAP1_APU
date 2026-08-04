#pragma once

#include "msap1/acquisition_ipc.hpp"

#include <cstdint>

namespace msap1::web {

/**
 * Typed application boundary between HTTP controllers and acquisition IPC.
 *
 * Controllers express product operations through this class.  They do not
 * construct MNCI frames, manage Unix sockets, or depend on correlation IDs.
 * The gateway deliberately returns the stable product response type so DTO
 * conversion remains an HTTP-controller concern.
 */
class AcquisitionGateway final {
public:
	AcquisitionGateway() = default;

	[[nodiscard]] AcquisitionResponse information(int timeout_ms = 1000);
	[[nodiscard]] AcquisitionResponse waveform_status(int timeout_ms = 1000);
	[[nodiscard]] AcquisitionResponse trigger_waveform(
		std::uint32_t pretrigger_ms, std::uint32_t posttrigger_ms,
		WaveformTriggerSource source, int timeout_ms = 3000);
	[[nodiscard]] AcquisitionResponse delete_waveform(
		std::uint64_t session_id, int timeout_ms = 3000);
	[[nodiscard]] AcquisitionResponse frequency_configuration(
		int timeout_ms = 1000);
	[[nodiscard]] AcquisitionResponse set_frequency_configuration(
		const FrequencyIpcConfiguration &configuration,
		int timeout_ms = 5000);
	[[nodiscard]] AcquisitionResponse adc_source(int timeout_ms = 1000);
	[[nodiscard]] AcquisitionResponse set_adc_source(
		std::uint32_t source, int timeout_ms = 5000);
	[[nodiscard]] AcquisitionResponse simulator_configuration(
		int timeout_ms = 1000);
	[[nodiscard]] AcquisitionResponse set_simulator_configuration(
		const SimulatorIpcConfiguration &configuration,
		int timeout_ms = 5000);
	[[nodiscard]] AcquisitionResponse set_capture(
		bool enabled, int timeout_ms = 3000);

private:
	AcquisitionClient client_;
};

} // namespace msap1::web
