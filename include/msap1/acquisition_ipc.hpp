#ifndef MSAP1_ACQUISITION_IPC_HPP
#define MSAP1_ACQUISITION_IPC_HPP

#include "msap1/meter_record.hpp"
#include "msap1/meter_data.hpp"
#include "msap1/rpu_control_protocol.h"
#include "msap1/waveform_capture.hpp"
#include "mnc/ipc/ipc.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <limits>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace msap1 {

inline constexpr const char *acquisition_socket_path =
	"/run/monutchee/fpga-acquisition.sock";
inline constexpr std::uint32_t acquisition_ipc_magic = 0x4d534151u;
inline constexpr std::uint16_t acquisition_ipc_version = 15;
inline constexpr std::uint32_t meter_record_stale_after_ms = 1000;
inline constexpr std::uint32_t waveform_duration_unspecified =
	std::numeric_limits<std::uint32_t>::max();

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
	waveform_delete = 13,
	adc_source_get = 14,
	adc_source_set = 15,
	adc_simulator_get = 16,
	adc_simulator_set = 17,
	meter_latest = 18,
	meter_stream_read = 19,
	meter_stream_register = 20,
	meter_stream_acknowledge = 21,
	meter_subscribe = 22,
	meter_unsubscribe = 23,
	configuration_apply = 24,
};

inline constexpr std::size_t acquisition_consumer_name_max = 64;
inline constexpr std::size_t acquisition_stream_read_max = 64;

struct MeterReadingIpc {
	std::int64_t value = 0;
	MeasurementQuality quality = MeasurementQuality::unavailable;
	std::uint64_t source_sequence = 0;
	std::int64_t measured_at_nanoseconds = 0;
	std::uint32_t window_sample_count = 0;
	std::uint64_t window_nanoseconds = 0;
};

struct FundamentalValuesIpc {
	MeterReadingIpc frequency{};
	std::array<MeterReadingIpc, 3> voltage_ln{};
	std::array<MeterReadingIpc, 4> current{};
};

struct MeterPeriodViewIpc {
	std::uint32_t available = 0;
	UpdatePeriod period = UpdatePeriod::ms200;
	std::uint64_t latest_sequence = 0;
	std::uint32_t configuration_generation = 0;
	std::int64_t updated_at_nanoseconds = 0;
	FundamentalValuesIpc fundamental{};
};

struct MeterStreamRecordIpc {
	std::uint64_t cursor = 0;
	std::int64_t received_at_nanoseconds = 0;
	MeterRecord record{};
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

struct SimulatorIpcChannel {
	double rms = 0.0;
	double phase_degrees = 0.0;
};

struct SimulatorIpcConfiguration {
	std::uint32_t frequency_millihz = 60000;
	std::uint32_t reserved = 0;
	std::array<SimulatorIpcChannel, 8> channels{};
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
	std::uint32_t waveform_pretrigger_ms = waveform_duration_unspecified;
	std::uint32_t waveform_posttrigger_ms = waveform_duration_unspecified;
	WaveformTriggerSource waveform_trigger_source =
		WaveformTriggerSource::manual_cli;
	std::uint64_t waveform_session_id = 0;
	std::uint32_t adc_source = MSAP1_ADC_SOURCE_PHYSICAL;
	std::uint32_t adc_source_reserved = 0;
	FrequencyIpcConfiguration frequency{};
	SimulatorIpcConfiguration simulator{};
	UpdatePeriod meter_period = UpdatePeriod::ms200;
	std::uint64_t meter_cursor = 0;
	std::uint32_t meter_limit = 32;
	std::string meter_consumer;
	std::string configuration_json;
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
	std::uint32_t adc_source = MSAP1_ADC_SOURCE_PHYSICAL;
	std::uint32_t adc_source_reserved = 0;
	SimulatorIpcConfiguration simulator{};
	WaveformStatus waveform{};
	std::uint32_t waveform_session_count = 0;
	std::uint32_t waveform_reserved = 0;
	std::array<WaveformSessionSummary, waveform_max_ipc_sessions>
		waveform_sessions{};
	MeterPeriodViewIpc meter_period_view{};
	std::uint64_t meter_next_cursor = 0;
	std::vector<MeterStreamRecordIpc> meter_stream_records;
};

[[nodiscard]] MeterPeriodViewIpc
to_ipc_period_view(const std::optional<MeterPeriodView> &view,
		   UpdatePeriod requested_period);

class AcquisitionUnavailable : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

class AcquisitionClient {
public:
	using EventHandler = std::function<void(const AcquisitionResponse &)>;
	explicit AcquisitionClient(
		std::string socket_path = acquisition_socket_path);
	~AcquisitionClient();
	AcquisitionClient(const AcquisitionClient &) = delete;
	AcquisitionClient &operator=(const AcquisitionClient &) = delete;
	AcquisitionClient(AcquisitionClient &&) noexcept;
	AcquisitionClient &operator=(AcquisitionClient &&) noexcept;
	AcquisitionResponse request(AcquisitionCommand command,
				    int timeout_ms = 3000,
				    const FrequencyIpcConfiguration *frequency =
					    nullptr,
				    std::uint32_t sample_rate_hz = 0,
				    std::uint32_t diagnostic_flow = 0,
				    std::uint32_t waveform_pretrigger_ms =
					    waveform_duration_unspecified,
				    std::uint32_t waveform_posttrigger_ms =
					    waveform_duration_unspecified,
				    WaveformTriggerSource waveform_trigger_source =
					    WaveformTriggerSource::manual_cli,
				    std::uint64_t waveform_session_id = 0,
				    std::uint32_t adc_source =
					    MSAP1_ADC_SOURCE_PHYSICAL,
				    const SimulatorIpcConfiguration *simulator =
					    nullptr);
	AcquisitionResponse request(AcquisitionRequest request,
				    int timeout_ms = 3000);
	void set_event_handler(EventHandler handler);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

[[nodiscard]] mnc::ipc::Frame
encode_acquisition_request(const AcquisitionRequest &request);
[[nodiscard]] AcquisitionRequest
decode_acquisition_request(const mnc::ipc::Frame &frame);
[[nodiscard]] mnc::ipc::Frame
encode_acquisition_response(const AcquisitionResponse &response,
			    std::uint32_t message_type);
[[nodiscard]] AcquisitionResponse
decode_acquisition_response(const mnc::ipc::Frame &frame);

/**
 * Typed MSAP1 acquisition protocol boundary.
 *
 * `mnc::ipc` owns stream framing only. This codec owns product message types,
 * little-endian payload serialization, validation, and IPC-v14 compatibility.
 * The free functions above remain as source-compatible implementation hooks.
 */
namespace acquisition {

class ProtocolCodec final {
public:
	[[nodiscard]] static mnc::ipc::Frame encode_request(
		const AcquisitionRequest &request)
	{
		return encode_acquisition_request(request);
	}

	[[nodiscard]] static AcquisitionRequest decode_request(
		const mnc::ipc::Frame &frame)
	{
		return decode_acquisition_request(frame);
	}

	[[nodiscard]] static mnc::ipc::Frame encode_response(
		const AcquisitionResponse &response, std::uint32_t message_type)
	{
		return encode_acquisition_response(response, message_type);
	}

	[[nodiscard]] static AcquisitionResponse decode_response(
		const mnc::ipc::Frame &frame)
	{
		return decode_acquisition_response(frame);
	}
};

} // namespace acquisition

} // namespace msap1

#endif
