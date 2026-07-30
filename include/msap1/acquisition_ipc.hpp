#ifndef MSAP1_ACQUISITION_IPC_HPP
#define MSAP1_ACQUISITION_IPC_HPP

#include "msap1/meter_record.hpp"
#include "msap1/rpu_control_protocol.h"
#include "msap1/waveform_capture.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace msap1 {

inline constexpr const char *acquisition_socket_path =
	"/run/monutchee/fpga-acquisition.sock";
inline constexpr std::uint32_t acquisition_ipc_magic = 0x4d534151u;
inline constexpr std::uint16_t acquisition_ipc_version = 10;
inline constexpr std::uint32_t meter_record_stale_after_ms = 1000;

enum class AcquisitionCommand : std::uint16_t {
	info = 1,
	health = 2,
	start = 3,
	stop = 4,
	frequency_configuration_get = 5,
	frequency_configuration_set = 6,
	sample_rate_set = 7,
	adc_diagnostic_run = 8,
	health_refresh = 9,
	waveform_status = 10,
	waveform_trigger = 11,
	waveform_list = 12,
};

struct FrequencyIpcConfiguration {
	std::uint32_t enabled = 1;
	std::uint32_t reference_channel = 6;
	std::uint32_t mode = MSAP1_FREQUENCY_MODE_ROLLING_CYCLES;
	std::uint32_t averaging_cycles = 10;
	std::uint32_t averaging_window_ms = 1000;
	std::uint32_t minimum_millihz = 40000;
	std::uint32_t maximum_millihz = 70000;
	std::uint32_t hysteresis_microvolts = 1000000;
};

enum class AcquisitionStatus : std::uint32_t {
	ok = 0,
	bad_request = 1,
	not_running = 2,
	dma_error = 3,
	rpu_error = 4,
	internal_error = 5,
	configuration_error = 6,
};

struct AcquisitionRequest {
	std::uint32_t magic = acquisition_ipc_magic;
	std::uint16_t version = acquisition_ipc_version;
	AcquisitionCommand command = AcquisitionCommand::info;
	std::uint64_t sequence = 0;
	std::uint32_t sample_rate_hz = 0;
	std::uint32_t diagnostic_flow = 0;
	std::uint32_t waveform_pretrigger_ms = 10000;
	std::uint32_t waveform_posttrigger_ms = 10000;
	WaveformTriggerSource waveform_trigger_source =
		WaveformTriggerSource::manual_cli;
	FrequencyIpcConfiguration frequency{};
};

struct AcquisitionResponse {
	std::uint32_t magic = acquisition_ipc_magic;
	std::uint16_t version = acquisition_ipc_version;
	std::uint16_t reserved = 0;
	AcquisitionStatus status = AcquisitionStatus::ok;
	std::uint32_t running = 0;
	std::uint32_t has_meter_record = 0;
	std::uint32_t sample_rate_hz = 32000;
	std::uint32_t meter_record_size = sizeof(MeterRecord);
	std::uint32_t configuration_generation = 0;
	std::uint64_t sequence = 0;
	std::uint64_t meter_records = 0;
	std::uint64_t dma_bytes = 0;
	std::uint64_t dma_read_errors = 0;
	std::uint64_t invalid_records = 0;
	std::uint64_t sequence_gaps = 0;
	std::uint32_t meter_record_age_ms = ~std::uint32_t{0};
	std::uint32_t rpu_health_age_ms = ~std::uint32_t{0};
	std::uint32_t health_probe_failures = 0;
	std::uint32_t health_probe_pending = 0;
	msap1_adc_health_payload rpu_health{};
	msap1_adc_diagnostic_payload adc_diagnostic{};
	MeterRecord latest_record{};
	FrequencyIpcConfiguration frequency{};
	WaveformStatus waveform{};
	std::uint32_t waveform_session_count = 0;
	std::uint32_t waveform_reserved = 0;
	std::array<WaveformSessionSummary, waveform_max_ipc_sessions>
		waveform_sessions{};
};

class AcquisitionUnavailable : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

class AcquisitionClient {
public:
	explicit AcquisitionClient(
		std::string socket_path = acquisition_socket_path);
	AcquisitionResponse request(AcquisitionCommand command,
				    int timeout_ms = 3000,
				    const FrequencyIpcConfiguration *frequency =
					    nullptr,
				    std::uint32_t sample_rate_hz = 0,
				    std::uint32_t diagnostic_flow = 0,
				    std::uint32_t waveform_pretrigger_ms = 10000,
				    std::uint32_t waveform_posttrigger_ms = 10000,
				    WaveformTriggerSource waveform_trigger_source =
					    WaveformTriggerSource::manual_cli) const;

private:
	std::string socket_path_;
};

} // namespace msap1

#endif
