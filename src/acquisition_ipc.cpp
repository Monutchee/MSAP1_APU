#include "msap1/acquisition_ipc.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/use_future.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <cmath>
#include <memory>
#include <mutex>
#include <thread>
#include <span>

namespace msap1 {
namespace {

using mnc::ipc::ByteReader;
using mnc::ipc::ByteWriter;

void write_frequency(ByteWriter &writer, const FrequencyIpcConfiguration &value)
{
	writer.u32(value.enabled);
	writer.u32(value.reference_channel);
	writer.u32(value.mode);
	writer.u32(value.averaging_cycles);
	writer.u32(value.averaging_window_ms);
	writer.u32(value.minimum_millihz);
	writer.u32(value.maximum_millihz);
	writer.u32(value.hysteresis_microvolts);
}

FrequencyIpcConfiguration read_frequency(ByteReader &reader)
{
	return {reader.u32(), reader.u32(), reader.u32(), reader.u32(),
		reader.u32(), reader.u32(), reader.u32(), reader.u32()};
}

void write_simulator(ByteWriter &writer, const SimulatorIpcConfiguration &value)
{
	writer.u32(value.frequency_millihz);
	writer.u32(value.reserved);
	for (const auto &channel : value.channels) {
		/* The in-process model uses doubles for UI input, but the wire ABI is
		 * integer-only and independent of compiler floating-point layout. */
		writer.i64(static_cast<std::int64_t>(
			std::llround(channel.rms * 1'000'000.0)));
		writer.i64(static_cast<std::int64_t>(
			std::llround(channel.phase_degrees * 1'000'000.0)));
	}
}

SimulatorIpcConfiguration read_simulator(ByteReader &reader)
{
	SimulatorIpcConfiguration value{};
	value.frequency_millihz = reader.u32();
	value.reserved = reader.u32();
	for (auto &channel : value.channels) {
		channel.rms = static_cast<double>(reader.i64()) / 1'000'000.0;
		channel.phase_degrees =
			static_cast<double>(reader.i64()) / 1'000'000.0;
	}
	return value;
}

void write_health(ByteWriter &writer, const msap1_adc_health_payload &value)
{
	writer.u32(value.health_flags);
	writer.u32(value.sample_rate_hz);
	writer.u32(value.capture_flags);
	writer.u32(value.frame_count);
	writer.u32(value.overflow_count);
	writer.u32(value.header_error_count);
	writer.u32(value.alert_count);
	writer.u32(value.packet_count);
	writer.u32(value.dclk_frequency_hz);
	writer.u32(value.drdy_frequency_hz);
	writer.u32(value.spi_error);
	writer.u16(value.expected_decimation);
	writer.u8(value.status_3);
	writer.u8(value.general_user_config_1);
	writer.u8(value.general_user_config_2);
	writer.u8(value.general_user_config_3);
	writer.u8(value.dout_format);
	writer.u8(value.src_n_msb);
	writer.u8(value.src_n_lsb);
	writer.u8(value.src_if_msb);
	writer.u8(value.src_if_lsb);
	writer.u8(value.src_update);
	writer.u32(value.meter_health_flags);
	writer.u32(value.meter_generation);
	writer.u32(value.conversion_status);
	writer.u32(value.processing_status);
	for (const auto item : value.channel_config)
		writer.u8(item);
	for (const auto item : value.channel_error)
		writer.u8(item);
	for (const auto item : value.saturation_error)
		writer.u8(item);
	writer.u8(value.channel_error_enable);
	writer.u8(value.general_error_1);
	writer.u8(value.general_error_1_enable);
	writer.u8(value.general_error_2);
	writer.u8(value.general_error_2_enable);
	writer.u8(value.status_1);
	writer.u8(value.status_2);
	writer.u8(value.channel_disable);
	for (const auto item : value.channel_sync_offset)
		writer.u8(item);
	writer.u8(value.adc_mux_config);
	writer.u8(value.global_mux_config);
	writer.u8(value.gpio_config);
	writer.u8(value.gpio_data);
	writer.u8(value.buffer_config_1);
	writer.u8(value.buffer_config_2);
	for (const auto &channel : value.channel_offset)
		for (const auto item : channel)
			writer.u8(item);
	for (const auto &channel : value.channel_gain)
		for (const auto item : channel)
			writer.u8(item);
	writer.u32(value.spi_protocol_error_count);
	writer.u32(value.spi_retry_recovery_count);
	writer.u8(value.spi_last_failed_register);
	writer.u8(value.spi_last_received_header);
	for (const auto item : value.spi_diagnostics_reserved)
		writer.u8(item);
	writer.u32(value.adc_source);
	writer.u32(value.simulator_status);
	writer.u32(value.simulator_active_generation);
	writer.u32(value.simulator_frame_count);
	writer.u32(value.simulator_saturation_count);
	writer.u32(value.simulator_missed_sample_count);
}

msap1_adc_health_payload read_health(ByteReader &reader)
{
	msap1_adc_health_payload value{};
	value.health_flags = reader.u32();
	value.sample_rate_hz = reader.u32();
	value.capture_flags = reader.u32();
	value.frame_count = reader.u32();
	value.overflow_count = reader.u32();
	value.header_error_count = reader.u32();
	value.alert_count = reader.u32();
	value.packet_count = reader.u32();
	value.dclk_frequency_hz = reader.u32();
	value.drdy_frequency_hz = reader.u32();
	value.spi_error = reader.u32();
	value.expected_decimation = reader.u16();
	value.status_3 = reader.u8();
	value.general_user_config_1 = reader.u8();
	value.general_user_config_2 = reader.u8();
	value.general_user_config_3 = reader.u8();
	value.dout_format = reader.u8();
	value.src_n_msb = reader.u8();
	value.src_n_lsb = reader.u8();
	value.src_if_msb = reader.u8();
	value.src_if_lsb = reader.u8();
	value.src_update = reader.u8();
	value.meter_health_flags = reader.u32();
	value.meter_generation = reader.u32();
	value.conversion_status = reader.u32();
	value.processing_status = reader.u32();
	for (auto &item : value.channel_config)
		item = reader.u8();
	for (auto &item : value.channel_error)
		item = reader.u8();
	for (auto &item : value.saturation_error)
		item = reader.u8();
	value.channel_error_enable = reader.u8();
	value.general_error_1 = reader.u8();
	value.general_error_1_enable = reader.u8();
	value.general_error_2 = reader.u8();
	value.general_error_2_enable = reader.u8();
	value.status_1 = reader.u8();
	value.status_2 = reader.u8();
	value.channel_disable = reader.u8();
	for (auto &item : value.channel_sync_offset)
		item = reader.u8();
	value.adc_mux_config = reader.u8();
	value.global_mux_config = reader.u8();
	value.gpio_config = reader.u8();
	value.gpio_data = reader.u8();
	value.buffer_config_1 = reader.u8();
	value.buffer_config_2 = reader.u8();
	for (auto &channel : value.channel_offset)
		for (auto &item : channel)
			item = reader.u8();
	for (auto &channel : value.channel_gain)
		for (auto &item : channel)
			item = reader.u8();
	value.spi_protocol_error_count = reader.u32();
	value.spi_retry_recovery_count = reader.u32();
	value.spi_last_failed_register = reader.u8();
	value.spi_last_received_header = reader.u8();
	for (auto &item : value.spi_diagnostics_reserved)
		item = reader.u8();
	value.adc_source = reader.u32();
	value.simulator_status = reader.u32();
	value.simulator_active_generation = reader.u32();
	value.simulator_frame_count = reader.u32();
	value.simulator_saturation_count = reader.u32();
	value.simulator_missed_sample_count = reader.u32();
	return value;
}

void write_snapshot(ByteWriter &writer,
		    const msap1_adc_diagnostic_snapshot &value)
{
	writer.u32(value.snapshot_flags);
	writer.u32(value.capture_flags);
	writer.u32(value.frame_count);
	writer.u32(value.packet_count);
	writer.u32(value.dclk_frequency_hz);
	writer.u32(value.drdy_frequency_hz);
	writer.u8(value.status_1);
	writer.u8(value.status_2);
	writer.u8(value.status_3);
	writer.u8(value.general_user_config_1);
	writer.u8(value.general_user_config_2);
	writer.u8(value.general_user_config_3);
	writer.u8(value.dout_format);
	writer.u8(value.channel_disable);
	writer.u8(value.buffer_config_1);
	writer.u8(value.buffer_config_2);
	writer.u8(value.src_n_msb);
	writer.u8(value.src_n_lsb);
	writer.u8(value.src_if_msb);
	writer.u8(value.src_if_lsb);
	writer.u8(value.src_update);
	writer.u8(value.reserved);
}

msap1_adc_diagnostic_snapshot read_snapshot(ByteReader &reader)
{
	msap1_adc_diagnostic_snapshot value{};
	value.snapshot_flags = reader.u32();
	value.capture_flags = reader.u32();
	value.frame_count = reader.u32();
	value.packet_count = reader.u32();
	value.dclk_frequency_hz = reader.u32();
	value.drdy_frequency_hz = reader.u32();
	value.status_1 = reader.u8();
	value.status_2 = reader.u8();
	value.status_3 = reader.u8();
	value.general_user_config_1 = reader.u8();
	value.general_user_config_2 = reader.u8();
	value.general_user_config_3 = reader.u8();
	value.dout_format = reader.u8();
	value.channel_disable = reader.u8();
	value.buffer_config_1 = reader.u8();
	value.buffer_config_2 = reader.u8();
	value.src_n_msb = reader.u8();
	value.src_n_lsb = reader.u8();
	value.src_if_msb = reader.u8();
	value.src_if_lsb = reader.u8();
	value.src_update = reader.u8();
	value.reserved = reader.u8();
	return value;
}

void write_diagnostic(ByteWriter &writer,
		      const msap1_adc_diagnostic_payload &value)
{
	writer.u32(value.flow);
	writer.u32(value.requested_sample_rate_hz);
	writer.u32(value.diagnostic_flags);
	writer.u32(value.diagnostic_error);
	writer.u32(value.failure_stage);
	writer.u32(value.reset_hold_ms);
	writer.u8(value.src_update_high_readback);
	writer.u8(value.src_update_low_readback);
	writer.u8(value.reserved[0]);
	writer.u8(value.reserved[1]);
	write_snapshot(writer, value.before);
	write_snapshot(writer, value.reset_asserted);
	write_snapshot(writer, value.reset_defaults);
	write_snapshot(writer, value.after);
}

msap1_adc_diagnostic_payload read_diagnostic(ByteReader &reader)
{
	msap1_adc_diagnostic_payload value{};
	value.flow = reader.u32();
	value.requested_sample_rate_hz = reader.u32();
	value.diagnostic_flags = reader.u32();
	value.diagnostic_error = reader.u32();
	value.failure_stage = reader.u32();
	value.reset_hold_ms = reader.u32();
	value.src_update_high_readback = reader.u8();
	value.src_update_low_readback = reader.u8();
	value.reserved[0] = reader.u8();
	value.reserved[1] = reader.u8();
	value.before = read_snapshot(reader);
	value.reset_asserted = read_snapshot(reader);
	value.reset_defaults = read_snapshot(reader);
	value.after = read_snapshot(reader);
	return value;
}

void write_meter_record(ByteWriter &writer, const MeterRecord &value)
{
	for (const auto word : value.words)
		writer.u32(word);
}

MeterRecord read_meter_record(ByteReader &reader)
{
	MeterRecord value{};
	for (auto &word : value.words)
		word = reader.u32();
	return value;
}

void write_meter_reading(ByteWriter &writer, const MeterReadingIpc &value)
{
	writer.i64(value.value);
	writer.u8(static_cast<std::uint8_t>(value.quality));
	writer.u8(0);
	writer.u16(0);
	writer.u64(value.source_sequence);
	writer.i64(value.measured_at_nanoseconds);
	writer.u32(value.window_sample_count);
	writer.u32(0);
	writer.u64(value.window_nanoseconds);
}

MeterReadingIpc read_meter_reading(ByteReader &reader)
{
	MeterReadingIpc value{};
	value.value = reader.i64();
	value.quality = static_cast<MeasurementQuality>(reader.u8());
	(void)reader.u8();
	(void)reader.u16();
	value.source_sequence = reader.u64();
	value.measured_at_nanoseconds = reader.i64();
	value.window_sample_count = reader.u32();
	(void)reader.u32();
	value.window_nanoseconds = reader.u64();
	return value;
}

void write_period_view(ByteWriter &writer, const MeterPeriodViewIpc &value)
{
	writer.u32(value.available);
	writer.u8(static_cast<std::uint8_t>(value.period));
	writer.u8(0);
	writer.u16(0);
	writer.u64(value.latest_sequence);
	writer.u32(value.configuration_generation);
	writer.u32(0);
	writer.i64(value.updated_at_nanoseconds);
	write_meter_reading(writer, value.fundamental.frequency);
	for (const auto &reading : value.fundamental.voltage_ln)
		write_meter_reading(writer, reading);
	for (const auto &reading : value.fundamental.current)
		write_meter_reading(writer, reading);
}

MeterPeriodViewIpc read_period_view(ByteReader &reader)
{
	MeterPeriodViewIpc value{};
	value.available = reader.u32();
	value.period = static_cast<UpdatePeriod>(reader.u8());
	(void)reader.u8();
	(void)reader.u16();
	value.latest_sequence = reader.u64();
	value.configuration_generation = reader.u32();
	(void)reader.u32();
	value.updated_at_nanoseconds = reader.i64();
	value.fundamental.frequency = read_meter_reading(reader);
	for (auto &reading : value.fundamental.voltage_ln)
		reading = read_meter_reading(reader);
	for (auto &reading : value.fundamental.current)
		reading = read_meter_reading(reader);
	return value;
}

void write_stream_record(ByteWriter &writer, const MeterStreamRecordIpc &value)
{
	writer.u64(value.cursor);
	writer.i64(value.received_at_nanoseconds);
	write_meter_record(writer, value.record);
}

MeterStreamRecordIpc read_stream_record(ByteReader &reader)
{
	MeterStreamRecordIpc value{};
	value.cursor = reader.u64();
	value.received_at_nanoseconds = reader.i64();
	value.record = read_meter_record(reader);
	return value;
}

void write_waveform_status(ByteWriter &writer, const WaveformStatus &value)
{
	writer.u32(value.running);
	writer.u32(value.active_session);
	writer.u32(value.sample_rate_hz);
	writer.u32(value.transport_ring_blocks);
	writer.u64(value.blocks);
	writer.u64(value.frames);
	writer.u64(value.bytes);
	writer.u64(value.invalid_blocks);
	writer.u64(value.sequence_gaps);
	writer.u64(value.transport_overrun_blocks);
	writer.u64(value.materialization_failures);
	writer.u64(value.history_oldest_sequence);
	writer.u64(value.history_latest_sequence);
	writer.u64(value.history_capacity_frames);
	writer.u64(value.completed_sessions);
	writer.u64(value.incomplete_sessions);
	writer.u64(value.correlation.tai_nanoseconds);
	writer.u64(value.correlation.pl_tick);
	writer.u64(value.correlation.frame_sequence);
	writer.u64(value.correlation.uncertainty_nanoseconds);
}

WaveformStatus read_waveform_status(ByteReader &reader)
{
	WaveformStatus value{};
	value.running = reader.u32();
	value.active_session = reader.u32();
	value.sample_rate_hz = reader.u32();
	value.transport_ring_blocks = reader.u32();
	value.blocks = reader.u64();
	value.frames = reader.u64();
	value.bytes = reader.u64();
	value.invalid_blocks = reader.u64();
	value.sequence_gaps = reader.u64();
	value.transport_overrun_blocks = reader.u64();
	value.materialization_failures = reader.u64();
	value.history_oldest_sequence = reader.u64();
	value.history_latest_sequence = reader.u64();
	value.history_capacity_frames = reader.u64();
	value.completed_sessions = reader.u64();
	value.incomplete_sessions = reader.u64();
	value.correlation.tai_nanoseconds = reader.u64();
	value.correlation.pl_tick = reader.u64();
	value.correlation.frame_sequence = reader.u64();
	value.correlation.uncertainty_nanoseconds = reader.u64();
	return value;
}

void write_waveform_session(ByteWriter &writer,
			    const WaveformSessionSummary &value)
{
	writer.u64(value.id);
	writer.u64(value.trigger_sequence);
	writer.u64(value.first_sequence);
	writer.u64(value.last_sequence);
	writer.u64(value.trigger_tai_nanoseconds);
	writer.u64(value.trigger_realtime_nanoseconds);
	writer.u32(value.sample_rate_hz);
	writer.u32(value.event_count);
	writer.u32(static_cast<std::uint32_t>(value.state));
	writer.u32(value.reserved);
	writer.fixed_string(value.filename.data(), value.filename.size());
}

WaveformSessionSummary read_waveform_session(ByteReader &reader)
{
	WaveformSessionSummary value{};
	value.id = reader.u64();
	value.trigger_sequence = reader.u64();
	value.first_sequence = reader.u64();
	value.last_sequence = reader.u64();
	value.trigger_tai_nanoseconds = reader.u64();
	value.trigger_realtime_nanoseconds = reader.u64();
	value.sample_rate_hz = reader.u32();
	value.event_count = reader.u32();
	value.state = static_cast<WaveformSessionState>(reader.u32());
	value.reserved = reader.u32();
	const auto filename = reader.fixed_string(value.filename.size());
	std::memcpy(value.filename.data(), filename.data(),
		    std::min(filename.size(), value.filename.size() - 1));
	return value;
}

} // namespace

MeterPeriodViewIpc
to_ipc_period_view(const std::optional<MeterPeriodView> &view,
		   UpdatePeriod requested_period)
{
	MeterPeriodViewIpc result{};
	result.period = requested_period;
	if (!view)
		return result;
	result.available = 1;
	result.period = view->period;
	result.latest_sequence = view->latest_sequence;
	result.configuration_generation = view->configuration_generation;
	result.updated_at_nanoseconds =
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			view->updated_at.time_since_epoch()).count();
	auto convert = [](const auto &reading) {
		return MeterReadingIpc{
			reading.value,
			reading.quality,
			reading.source_sequence,
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				reading.measured_at.time_since_epoch()).count(),
			reading.calculation_window.sample_count,
			static_cast<std::uint64_t>(
				reading.calculation_window.duration.count())};
	};
	const auto &fundamental = view->values.fundamental;
	result.fundamental.frequency = convert(fundamental.frequency);
	result.fundamental.voltage_ln = {
		convert(fundamental.voltage_ln.phase_a),
		convert(fundamental.voltage_ln.phase_b),
		convert(fundamental.voltage_ln.phase_c)};
	result.fundamental.current = {
		convert(fundamental.current.phase_a),
		convert(fundamental.current.phase_b),
		convert(fundamental.current.phase_c),
		convert(fundamental.current.neutral)};
	return result;
}

mnc::ipc::Frame encode_acquisition_request(const AcquisitionRequest &request)
{
	ByteWriter writer;
	writer.u16(acquisition_ipc_version);
	writer.u16(static_cast<std::uint16_t>(request.command));
	writer.u32(request.sample_rate_hz);
	writer.u32(request.diagnostic_flow);
	writer.u32(request.waveform_pretrigger_ms);
	writer.u32(request.waveform_posttrigger_ms);
	writer.u32(static_cast<std::uint32_t>(request.waveform_trigger_source));
	writer.u64(request.waveform_session_id);
	writer.u32(request.adc_source);
	writer.u32(request.adc_source_reserved);
	write_frequency(writer, request.frequency);
	write_simulator(writer, request.simulator);
	writer.u8(static_cast<std::uint8_t>(request.meter_period));
	writer.u8(0);
	writer.u16(0);
	writer.u64(request.meter_cursor);
	writer.u32(request.meter_limit);
	writer.fixed_string(request.meter_consumer,
			    acquisition_consumer_name_max);
	if (request.configuration_json.size() > mnc::ipc::default_max_payload / 2u)
		throw std::length_error("acquisition configuration JSON is oversized");
	writer.u32(static_cast<std::uint32_t>(request.configuration_json.size()));
	writer.bytes(std::as_bytes(std::span(request.configuration_json.data(),
		request.configuration_json.size())));
	return {mnc::ipc::FrameKind::request,
		static_cast<std::uint32_t>(request.command), request.sequence,
		writer.take()};
}

AcquisitionRequest decode_acquisition_request(const mnc::ipc::Frame &frame)
{
	if (frame.kind != mnc::ipc::FrameKind::request)
		throw std::invalid_argument("acquisition IPC frame is not a request");
	ByteReader reader(frame.payload);
	if (reader.u16() != acquisition_ipc_version)
		throw std::invalid_argument("unsupported acquisition IPC version");
	AcquisitionRequest request{};
	request.command = static_cast<AcquisitionCommand>(reader.u16());
	if (frame.message_type != static_cast<std::uint32_t>(request.command))
		throw std::invalid_argument("acquisition message type mismatch");
	request.sequence = frame.correlation_id;
	request.sample_rate_hz = reader.u32();
	request.diagnostic_flow = reader.u32();
	request.waveform_pretrigger_ms = reader.u32();
	request.waveform_posttrigger_ms = reader.u32();
	request.waveform_trigger_source =
		static_cast<WaveformTriggerSource>(reader.u32());
	request.waveform_session_id = reader.u64();
	request.adc_source = reader.u32();
	request.adc_source_reserved = reader.u32();
	request.frequency = read_frequency(reader);
	request.simulator = read_simulator(reader);
	request.meter_period = static_cast<UpdatePeriod>(reader.u8());
	(void)reader.u8();
	(void)reader.u16();
	request.meter_cursor = reader.u64();
	request.meter_limit = reader.u32();
	request.meter_consumer =
		reader.fixed_string(acquisition_consumer_name_max);
	const auto configuration_size = reader.u32();
	if (configuration_size > mnc::ipc::default_max_payload / 2u)
		throw std::invalid_argument("acquisition configuration JSON is oversized");
	const auto configuration = reader.bytes(configuration_size);
	request.configuration_json.assign(
		reinterpret_cast<const char *>(configuration.data()),
		configuration.size());
	reader.require_finished();
	return request;
}

mnc::ipc::Frame encode_acquisition_response(const AcquisitionResponse &response,
					     std::uint32_t message_type)
{
	ByteWriter writer;
	writer.u16(acquisition_ipc_version);
	writer.u16(0);
	writer.u32(static_cast<std::uint32_t>(response.status));
	writer.u32(response.running);
	writer.u32(response.has_meter_record);
	writer.u32(response.sample_rate_hz);
	writer.u32(response.meter_record_size);
	writer.u32(response.configuration_generation);
	writer.u64(response.meter_records);
	writer.u64(response.dma_bytes);
	writer.u64(response.dma_read_errors);
	writer.u64(response.invalid_records);
	writer.u64(response.sequence_gaps);
	writer.u32(response.meter_record_age_ms);
	writer.u32(response.rpu_health_age_ms);
	writer.u32(response.health_probe_failures);
	writer.u32(response.health_probe_pending);
	write_health(writer, response.rpu_health);
	write_diagnostic(writer, response.adc_diagnostic);
	write_meter_record(writer, response.latest_record);
	write_frequency(writer, response.frequency);
	writer.u32(response.adc_source);
	writer.u32(response.adc_source_reserved);
	write_simulator(writer, response.simulator);
	write_waveform_status(writer, response.waveform);
	writer.u32(response.waveform_session_count);
	writer.u32(response.waveform_reserved);
	for (const auto &session : response.waveform_sessions)
		write_waveform_session(writer, session);
	write_period_view(writer, response.meter_period_view);
	writer.u64(response.meter_next_cursor);
	writer.u32(static_cast<std::uint32_t>(response.meter_stream_records.size()));
	for (const auto &record : response.meter_stream_records)
		write_stream_record(writer, record);
	return {response.status == AcquisitionStatus::ok
			? mnc::ipc::FrameKind::response
			: mnc::ipc::FrameKind::error,
		message_type, response.sequence, writer.take()};
}

AcquisitionResponse decode_acquisition_response(const mnc::ipc::Frame &frame)
{
	if (frame.kind != mnc::ipc::FrameKind::response &&
	    frame.kind != mnc::ipc::FrameKind::error &&
	    frame.kind != mnc::ipc::FrameKind::event)
		throw std::invalid_argument("acquisition IPC frame is not a response");
	ByteReader reader(frame.payload);
	if (reader.u16() != acquisition_ipc_version)
		throw std::invalid_argument("unsupported acquisition IPC version");
	(void)reader.u16();
	AcquisitionResponse response{};
	response.sequence = frame.correlation_id;
	response.status = static_cast<AcquisitionStatus>(reader.u32());
	response.running = reader.u32();
	response.has_meter_record = reader.u32();
	response.sample_rate_hz = reader.u32();
	response.meter_record_size = reader.u32();
	response.configuration_generation = reader.u32();
	response.meter_records = reader.u64();
	response.dma_bytes = reader.u64();
	response.dma_read_errors = reader.u64();
	response.invalid_records = reader.u64();
	response.sequence_gaps = reader.u64();
	response.meter_record_age_ms = reader.u32();
	response.rpu_health_age_ms = reader.u32();
	response.health_probe_failures = reader.u32();
	response.health_probe_pending = reader.u32();
	response.rpu_health = read_health(reader);
	response.adc_diagnostic = read_diagnostic(reader);
	response.latest_record = read_meter_record(reader);
	response.frequency = read_frequency(reader);
	response.adc_source = reader.u32();
	response.adc_source_reserved = reader.u32();
	response.simulator = read_simulator(reader);
	response.waveform = read_waveform_status(reader);
	response.waveform_session_count = reader.u32();
	response.waveform_reserved = reader.u32();
	for (auto &session : response.waveform_sessions)
		session = read_waveform_session(reader);
	response.meter_period_view = read_period_view(reader);
	response.meter_next_cursor = reader.u64();
	const auto stream_count = reader.u32();
	if (stream_count > acquisition_stream_read_max)
		throw std::invalid_argument("meter stream response is oversized");
	response.meter_stream_records.reserve(stream_count);
	for (std::uint32_t index = 0; index < stream_count; ++index)
		response.meter_stream_records.push_back(read_stream_record(reader));
	reader.require_finished();
	return response;
}

struct AcquisitionClient::Impl {
	explicit Impl(std::string socket_path)
		: work(boost::asio::make_work_guard(context)),
		  client(std::make_shared<mnc::ipc::RequestClient>(
			  context.get_executor(), std::move(socket_path))),
		  thread([this] { context.run(); })
	{
		/* Connect lazily from request().  Besides allowing a persistent client
		 * to be constructed before the service is ready, this keeps transport
		 * failures inside the product-level AcquisitionUnavailable boundary. */
	}

	~Impl()
	{
		client->close();
		work.reset();
		context.stop();
		if (thread.joinable())
			thread.join();
	}

	boost::asio::io_context context;
	boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work;
	std::shared_ptr<mnc::ipc::RequestClient> client;
	AcquisitionClient::EventHandler event_handler;
	std::mutex connect_mutex;
	std::thread thread;
};

AcquisitionClient::AcquisitionClient(std::string socket_path)
	: impl_(std::make_unique<Impl>(std::move(socket_path)))
{
}

AcquisitionClient::~AcquisitionClient() = default;
AcquisitionClient::AcquisitionClient(AcquisitionClient &&) noexcept = default;
AcquisitionClient &AcquisitionClient::operator=(AcquisitionClient &&) noexcept =
	default;

AcquisitionResponse AcquisitionClient::request(
	AcquisitionCommand command, int timeout_ms,
	const FrequencyIpcConfiguration *frequency, std::uint32_t sample_rate_hz,
	std::uint32_t diagnostic_flow, std::uint32_t waveform_pretrigger_ms,
	std::uint32_t waveform_posttrigger_ms,
	WaveformTriggerSource waveform_trigger_source,
	std::uint64_t waveform_session_id, std::uint32_t adc_source,
	const SimulatorIpcConfiguration *simulator)
{
	AcquisitionRequest request{};
	request.command = command;
	request.sample_rate_hz = sample_rate_hz;
	request.diagnostic_flow = diagnostic_flow;
	request.waveform_pretrigger_ms = waveform_pretrigger_ms;
	request.waveform_posttrigger_ms = waveform_posttrigger_ms;
	request.waveform_trigger_source = waveform_trigger_source;
	request.waveform_session_id = waveform_session_id;
	request.adc_source = adc_source;
	if (frequency)
		request.frequency = *frequency;
	if (simulator)
		request.simulator = *simulator;
	return this->request(std::move(request), timeout_ms);
}

AcquisitionResponse AcquisitionClient::request(AcquisitionRequest request,
						 int timeout_ms)
{
	/* Correlation IDs belong to the transport.  A timestamp generated here can
	 * collide when parallel HTTP handlers submit requests in the same clock
	 * tick, which previously surfaced as "duplicate IPC correlation ID". */
	request.sequence = 0;
	try {
		if (!impl_->client->is_open()) {
			/* Only one caller establishes the persistent stream. Requests remain
			 * concurrent after connection because RequestClient serializes its
			 * own frame queue and correlation map on an Asio strand. */
			std::scoped_lock connect_lock(impl_->connect_mutex);
			if (!impl_->client->is_open())
				boost::asio::co_spawn(impl_->context,
					impl_->client->connect(), boost::asio::use_future)
					.get();
		}
		auto frame = boost::asio::co_spawn(
			impl_->context,
			impl_->client->request(
				acquisition::ProtocolCodec::encode_request(request),
				std::chrono::milliseconds(timeout_ms)),
			boost::asio::use_future)
			.get();
		if (frame.message_type !=
			    static_cast<std::uint32_t>(request.command))
			throw std::runtime_error("invalid acquisition response identity");
		return acquisition::ProtocolCodec::decode_response(frame);
	} catch (const std::exception &error) {
		/* An EOF or reset can leave the native socket looking open until the
		 * reader coroutine observes it. Explicit invalidation makes the next
		 * product request establish a fresh stream. Side-effecting requests are
		 * not retried here because the lost response may follow a successful
		 * operation on the server. */
		impl_->client->close();
		throw AcquisitionUnavailable(error.what());
	}
}

void AcquisitionClient::set_event_handler(EventHandler handler)
{
	impl_->event_handler = std::move(handler);
	impl_->client->set_event_handler([impl = impl_.get()](mnc::ipc::Frame frame) {
		if (!impl->event_handler)
			return;
		try {
			impl->event_handler(
				acquisition::ProtocolCodec::decode_response(frame));
		} catch (...) {
			/* A malformed event closes no request path. Product code can
			 * detect missed updates by sequence and request a fresh snapshot. */
		}
	});
}

} // namespace msap1
