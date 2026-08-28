#include "msap1/meter/meter_config.hpp"
#include "msap1/meter/meter_health.hpp"
#include "msap1/meter/meter_record.hpp"
#include "msap1/acquisition/rpu/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

#include <unistd.h>

namespace {

void require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

template <typename Function>
void require_throws(Function function, const char *message)
{
	try {
		function();
	} catch (const std::exception &) {
		return;
	}
	throw std::runtime_error(message);
}

void request_round_trip()
{
	const auto wire = msap1::encode_request(
		MSAP1_RPU_MSG_ADC_CAPTURE_START, 5);
	const auto decoded = msap1::decode_message(wire.data(), wire.size());
	require(decoded.header.type == MSAP1_RPU_MSG_ADC_CAPTURE_START,
		"wrong message type");
	require(decoded.header.sequence == 5, "wrong message sequence");
	require(decoded.payload.empty(), "capture start must have no payload");
}

void meter_ack_round_trip()
{
	msap1_meter_config_ack_payload acknowledgement{};
	acknowledgement.generation = 0x12345678;
	acknowledgement.conversion_active_generation = 0x12345678;
	acknowledgement.processing_active_generation = 0x12345678;
	acknowledgement.conversion_status = 1;
	acknowledgement.processing_status = 5;
	const auto wire = msap1::encode_request(MSAP1_RPU_MSG_ACK, 17,
		&acknowledgement, sizeof(acknowledgement));
	const auto message = msap1::decode_message(wire.data(), wire.size());
	const auto decoded = msap1::decode_meter_config_ack(message);
	require(decoded.generation == acknowledgement.generation,
		"wrong meter acknowledgement generation");
	require(decoded.processing_status == 5,
		"wrong meter processing status");
}

void adc_health_round_trip()
{
	msap1_adc_health_payload health{};
	health.health_flags = MSAP1_ADC_HEALTH_SPI_RESPONSIVE |
		MSAP1_ADC_HEALTH_INITIALIZED | MSAP1_ADC_HEALTH_CONFIG_MATCH;
	health.meter_health_flags = MSAP1_METER_HEALTH_CONFIGURED |
		MSAP1_METER_HEALTH_GENERATION_MATCH;
	health.meter_generation = 0x11223344;
	health.sample_rate_hz = 32000;
	health.frame_count = 123456;
	health.packet_count = 482;
	health.dclk_frequency_hz = 8192000;
	health.drdy_frequency_hz = 32000;
	health.expected_decimation = 64;
	health.channel_config[6] = 0x80;
	health.channel_disable = 0x01;
	health.channel_sync_offset[6] = 0x23;
	health.adc_mux_config = 0x40;
	health.buffer_config_1 = 0x38;
	health.buffer_config_2 = 0xc0;
	health.channel_offset[6][0] = 0x11;
	health.channel_offset[6][1] = 0x22;
	health.channel_offset[6][2] = 0x33;
	health.channel_gain[6][0] = 0x44;
	health.channel_gain[6][1] = 0x55;
	health.channel_gain[6][2] = 0x66;
	health.channel_error[6] = 0x12;
	health.saturation_error[3] = 0x21;
	health.channel_error_enable = 0xfe;
	health.general_error_1 = 0x04;
	health.general_error_1_enable = 0x3e;
	health.general_error_2 = 0x20;
	health.general_error_2_enable = 0x3c;
	health.status_1 = 0x10;
	health.status_2 = 0x08;
	health.status_3 = 0x30;
	health.spi_protocol_error_count = 7;
	health.spi_retry_recovery_count = 3;
	health.spi_last_failed_register = 0x4d;
	health.spi_last_received_header = 0x00;

	const auto wire = msap1::encode_request(MSAP1_RPU_MSG_ADC_HEALTH,
		23, &health, sizeof(health));
	const auto message = msap1::decode_message(wire.data(), wire.size());
	const auto decoded = msap1::decode_adc_health(message);
	require(decoded.sample_rate_hz == 32000, "wrong health sample rate");
	require(decoded.frame_count == 123456, "wrong health frame count");
	require(decoded.packet_count == 482, "wrong health packet count");
	require(decoded.dclk_frequency_hz == 8192000,
		"wrong health DCLK frequency");
	require(decoded.drdy_frequency_hz == 32000,
		"wrong health DRDY frequency");
	require(decoded.meter_generation == 0x11223344,
		"wrong health meter generation");
	require(decoded.channel_config[6] == 0x80,
		"wrong health channel configuration");
	require(decoded.channel_disable == 0x01 &&
			decoded.channel_sync_offset[6] == 0x23,
		"wrong health channel controls");
	require(decoded.adc_mux_config == 0x40 &&
			decoded.buffer_config_1 == 0x38 &&
			decoded.buffer_config_2 == 0xc0,
		"wrong health common configuration");
	require(decoded.channel_offset[6][0] == 0x11 &&
			decoded.channel_offset[6][1] == 0x22 &&
			decoded.channel_offset[6][2] == 0x33 &&
			decoded.channel_gain[6][0] == 0x44 &&
			decoded.channel_gain[6][1] == 0x55 &&
			decoded.channel_gain[6][2] == 0x66,
		"wrong health calibration registers");
	require(decoded.channel_error[6] == 0x12,
		"wrong health channel error");
	require(decoded.saturation_error[3] == 0x21,
		"wrong health saturation error");
	require(decoded.general_error_1 == 0x04 &&
			decoded.general_error_2 == 0x20,
		"wrong health general errors");
	require(decoded.status_1 == 0x10 && decoded.status_2 == 0x08 &&
			decoded.status_3 == 0x30,
		"wrong health status registers");
	require(decoded.spi_protocol_error_count == 7 &&
			decoded.spi_retry_recovery_count == 3 &&
			decoded.spi_last_failed_register == 0x4d &&
			decoded.spi_last_received_header == 0x00,
		"wrong SPI health diagnostics");
}

void aggregation_health_round_trip()
{
	static_assert(sizeof(msap1_aggregation_health_payload) == 200,
		      "aggregation health ABI must be 50 packed words");
	msap1_aggregation_health_payload health{};
	health.health_flags = MSAP1_AGGREGATION_HEALTH_TRANSPORT_AVAILABLE |
		MSAP1_AGGREGATION_HEALTH_TRANSPORT_INITIALIZED |
		MSAP1_AGGREGATION_HEALTH_INPUT_HEALTHY |
		MSAP1_AGGREGATION_HEALTH_ENGINE_READY |
		MSAP1_AGGREGATION_HEALTH_OUTPUT_READY |
		MSAP1_AGGREGATION_HEALTH_OUTPUT_ACTIVE |
		MSAP1_AGGREGATION_HEALTH_AUTHORITATIVE;
	health.frames_received = 101;
	health.software_ring_push_failures = 2;
	health.input_records_dropped = 3;
	health.first_dropped_sequence = 41;
	health.last_dropped_sequence = 43;
	health.software_ring_current = 4;
	health.software_ring_high_water = 48;
	health.software_ring_capacity = 64;
	health.software_ring_pressure =
		MSAP1_AGGREGATION_RING_PRESSURE_WARNING;
	health.software_ring_warning_entries = 5;
	health.software_ring_high_entries = 6;
	health.software_ring_critical_entries = 7;
	health.software_ring_full_entries = 8;
	health.hardware_fifo_current_words = 9;
	health.hardware_fifo_high_water_words = 10;
	health.hardware_fifo_full_events = 11;
	health.input_wake_count = 12;
	health.input_records_processed = 13;
	health.input_max_batch = 4;
	health.input_max_runtime_us = 15;
	health.validator_wake_count = 16;
	health.validator_records_processed = 17;
	health.validator_max_runtime_us = 18;
	health.validator_max_schedule_gap_us = 19;

	const auto wire = msap1::encode_request(
		MSAP1_RPU_MSG_AGGREGATION_HEALTH, 37, &health,
		sizeof(health));
	const auto message = msap1::decode_message(wire.data(), wire.size());
	const auto decoded = msap1::decode_aggregation_health(message);
	require(std::memcmp(&decoded, &health, sizeof(health)) == 0,
		"aggregation health payload changed during wire round trip");
}

void adc_diagnostic_round_trip()
{
	msap1_adc_diagnostic_payload diagnostic{};
	diagnostic.flow = 1;
	diagnostic.requested_sample_rate_hz = 32000;
	diagnostic.diagnostic_flags =
		MSAP1_ADC_DIAGNOSTIC_RESET_ASSERTED |
		MSAP1_ADC_DIAGNOSTIC_SRC_UPDATE_HIGH_READ |
		MSAP1_ADC_DIAGNOSTIC_SRC_HOLDING_MATCH;
	diagnostic.reset_hold_ms = 2200;
	diagnostic.src_update_high_readback = 0x01;
	diagnostic.src_update_low_readback = 0x00;
	diagnostic.before.snapshot_flags =
		MSAP1_ADC_DIAGNOSTIC_SNAPSHOT_SPI_VALID;
	diagnostic.before.dclk_frequency_hz = 8192000;
	diagnostic.before.drdy_frequency_hz = 19200;
	diagnostic.reset_asserted.drdy_frequency_hz = 0;
	diagnostic.reset_defaults.status_3 = 0x30;
	diagnostic.after.src_n_lsb = 64;
	diagnostic.after.drdy_frequency_hz = 32000;

	const auto wire = msap1::encode_request(
		MSAP1_RPU_MSG_ADC_DIAGNOSTIC, 31,
		&diagnostic, sizeof(diagnostic));
	const auto message = msap1::decode_message(wire.data(), wire.size());
	const auto decoded = msap1::decode_adc_diagnostic(message);
	require(decoded.flow == 1 &&
			decoded.requested_sample_rate_hz == 32000,
		"wrong ADC diagnostic identity");
	require(decoded.before.drdy_frequency_hz == 19200 &&
			decoded.reset_asserted.drdy_frequency_hz == 0 &&
			decoded.after.drdy_frequency_hz == 32000,
		"wrong ADC diagnostic snapshots");
	require(decoded.src_update_high_readback == 0x01 &&
			decoded.after.src_n_lsb == 64,
		"wrong ADC diagnostic SRC trace");
}

void meter_config_wire_layout()
{
	/* Wire v7 expands each of the four simulator tone slots from two words
	 * to three: Q16.16 frequency ratio, lane-mask/Q16 amplitude, and Q0.32
	 * phase. The complete frame remains below the 384-byte RPMsg cap. */
	static_assert(sizeof(msap1_meter_config_payload) == 312,
		      "meter config payload must be 312 packed bytes");
	static_assert(offsetof(msap1_meter_config_payload,
			       simulator_dc_offset_counts) == 172,
		      "DC offsets must follow the simulator phase step");
	static_assert(offsetof(msap1_meter_config_payload,
			       simulator_noise_level_counts) == 204,
		      "noise levels must follow the DC offsets");
	static_assert(offsetof(msap1_meter_config_payload,
			       simulator_harmonics) == 236,
		      "harmonic slots must follow the noise levels");
	static_assert(offsetof(msap1_meter_config_payload,
			       simulator_flags) == 284,
		      "simulator flags must follow the harmonic slots");
	static_assert(offsetof(msap1_meter_config_payload,
			       nominal_frequency_hz) == 288,
		      "nominal_frequency_hz must follow the simulator flags");
	static_assert(offsetof(msap1_meter_config_payload,
			       pq_reference_microvolts) == 292,
		      "the PQ reference must follow the nominal frequency");
	static_assert(offsetof(msap1_meter_config_payload,
			       pq_hysteresis_e4) == 308,
		      "the PQ hysteresis must be the trailing field");

	msap1_meter_config_payload payload{};
	payload.nominal_frequency_hz = 50;
	payload.simulator_dc_offset_counts[2] = -12345;
	payload.simulator_noise_level_counts[6] = 777;
	/* Slot 0: 3.0x, 5% (0x0ccd Q16), voltage lanes, 90 degree phase. */
	payload.simulator_harmonics[0] = 0x00030000u;
	payload.simulator_harmonics[1] = 0x0ccd0070u;
	payload.simulator_harmonics[2] = 0x40000000u;
	payload.simulator_flags = MSAP1_SIMULATOR_FLAG_PRESERVE_PHASE;
	payload.pq_reference_microvolts = 230000000u;
	payload.pq_hysteresis_e4 = 200u;
	const auto wire = msap1::encode_request(MSAP1_RPU_MSG_METER_CONFIG_SET,
		7, &payload, sizeof(payload));
	const auto decoded = msap1::decode_message(wire.data(), wire.size());
	require(decoded.payload.size() == sizeof(payload),
		"meter config payload size changed on the wire");
	std::uint32_t nominal = 0;
	std::memcpy(&nominal, decoded.payload.data() + 288, sizeof(nominal));
	require(nominal == 50,
		"nominal frequency was not encoded at the trailing offset");
	std::int32_t dc = 0;
	std::memcpy(&dc, decoded.payload.data() + 172 + 2 * 4, sizeof(dc));
	require(dc == -12345,
		"DC offset was not encoded at its normative offset");
	std::uint32_t noise = 0;
	std::memcpy(&noise, decoded.payload.data() + 204 + 6 * 4, sizeof(noise));
	require(noise == 777,
		"noise level was not encoded at its normative offset");
	std::uint32_t harmonic = 0;
	std::memcpy(&harmonic, decoded.payload.data() + 236,
		    sizeof(harmonic));
	require(harmonic == 0x00030000u,
		"harmonic ratio was not encoded at its normative offset");
	std::memcpy(&harmonic, decoded.payload.data() + 236 + 4,
		    sizeof(harmonic));
	require(harmonic == 0x0ccd0070u,
		"harmonic amplitude/mask was not encoded at its normative offset");
	std::uint32_t flags = 0;
	std::memcpy(&flags, decoded.payload.data() + 284, sizeof(flags));
	require(flags == MSAP1_SIMULATOR_FLAG_PRESERVE_PHASE,
		"simulator flags were not encoded at their normative offset");
	std::uint32_t reference = 0;
	std::memcpy(&reference, decoded.payload.data() + 292, sizeof(reference));
	require(reference == 230000000u,
		"the PQ reference was not encoded at its normative offset");
	std::uint32_t hysteresis = 0;
	std::memcpy(&hysteresis, decoded.payload.data() + 308,
		    sizeof(hysteresis));
	require(hysteresis == 200u,
		"the PQ hysteresis was not encoded at its normative offset");
}

void simulator_event_wire_layout()
{
	/* Wire version 5: the event sequencer is its own message, so a burst
	 * never rides a configuration commit (which restarts capture). */
	static_assert(sizeof(msap1_simulator_event_payload) == 24,
		      "simulator event payload must be 24 packed bytes");
	static_assert(sizeof(msap1_simulator_event_ack_payload) == 20,
		      "simulator event ack must be 20 packed bytes");
	static_assert(offsetof(msap1_simulator_event_payload,
			       duration_half_cycles) == 12,
		      "the burst duration must follow the amplitude scale");

	msap1_simulator_event_payload payload{};
	payload.action = MSAP1_SIMULATOR_EVENT_ARM;
	payload.channel_mask = 0x70u;
	payload.scale_q16 = 0xE666u;
	payload.duration_half_cycles = 20u;
	payload.period_half_cycles = 200u;
	payload.flags = MSAP1_SIMULATOR_EVENT_FLAG_REPEAT;
	const auto wire = msap1::encode_request(
		MSAP1_RPU_MSG_SIMULATOR_EVENT_SET, 9, &payload, sizeof(payload));
	const auto decoded = msap1::decode_message(wire.data(), wire.size());
	require(decoded.payload.size() == sizeof(payload),
		"simulator event payload size changed on the wire");
	require(decoded.header.version == MSAP1_RPU_VERSION &&
			MSAP1_RPU_VERSION == 7u,
		"the simulator event message belongs to wire version 7");
	msap1_simulator_event_payload round_trip{};
	std::memcpy(&round_trip, decoded.payload.data(), sizeof(round_trip));
	require(round_trip.channel_mask == 0x70u &&
			round_trip.scale_q16 == 0xE666u &&
			round_trip.duration_half_cycles == 20u &&
			round_trip.period_half_cycles == 200u &&
			round_trip.flags == MSAP1_SIMULATOR_EVENT_FLAG_REPEAT,
		"simulator event fields did not survive the wire");
}

void meter_record_contract()
{
	msap1::MeterRecord record{};
	record.words[0] = msap1::meter_record_magic;
	record.words[1] = msap1::meter_periodic_format;
	record.words[2] = msap1::meter_record_size;
	record.words[3] = 41;
	record.words[4] = 0xabcdef01;
	record.words[5] = 32000;
	record.words[6] = 6400;
	record.words[7] = 1u << 6;
	record.words[9] = 0x00000010u;
	record.words[10] = 0x00000001u;
	record.words[11] = 3;
	record.words[12] = 4;
	record.words[13] = 60u | (12u << 8) | (1u << 16);
	record.words[60] = 5;
	record.words[61] = 6;
	record.words[62] = 7;
	record.words[63] = 8;
	const std::uint64_t mean = static_cast<std::uint64_t>(-125000);
	const std::uint64_t rms = 230123456;
	const std::size_t base = 16 + 6 * 5;
	record.words[base] = static_cast<std::uint32_t>(mean);
	record.words[base + 1] = static_cast<std::uint32_t>(mean >> 32);
	record.words[base + 2] = 777;
	record.words[base + 3] = static_cast<std::uint32_t>(rms);
	record.words[base + 4] = static_cast<std::uint32_t>(rms >> 32);
	record.words[56] = 60001;
	record.words[57] = (1u << 0) | (1u << 1) | (1u << 2) |
		(1u << 8) | (6u << 12) | (10u << 16);
	record.words[58] = 0x02155555;
	record.words[59] = 19;
	require(record.header_valid(), "valid meter header rejected");
	require(record.sequence() == 41, "wrong meter sequence");
	const auto channel = record.channel(6);
	require(channel.valid, "voltage channel is not valid");
	require(channel.mean_micro_units == -125000, "wrong signed mean");
	require(channel.rms_count == 777, "wrong RMS count");
	require(channel.rms_micro_units == 230123456, "wrong RMS voltage");
	const auto frequency = record.frequency();
	require(frequency.valid && frequency.millihz == 60001 &&
		frequency.mode == 1 && frequency.reference_channel == 6 &&
		frequency.cycles_used == 10 &&
		frequency.measurement_sequence == 19,
		"wrong frequency record decoding");
	/* v3 envelope positions: 64-bit first-sample index in words 9/10 and
	 * the transport drop words in 11/12. */
	require(record.first_sample_index() == 0x100000010ull,
		"wrong first-sample index words");
	require(record.emit_drops() == 3 && record.result_drops() == 4,
		"wrong transport drop words");
	const auto timing = record.timing();
	require(timing.nominal_frequency_hz == 60 && timing.cycle_count == 12 &&
		timing.cycle_locked && !timing.free_run_fallback,
		"wrong timing word decoding");
	/* MTR1 capture diagnostics latched at block close, words 60..63. */
	require(record.capture_frames() == 5 && record.header_errors() == 6 &&
		record.fifo_overflows() == 7 && record.adc_alerts() == 8,
		"wrong capture diagnostic words");
}

void meter_configuration()
{
	const auto path = std::filesystem::temp_directory_path() /
		("msap1-meter-config-" + std::to_string(::getpid()) + ".json");
	{
		std::ofstream output(path);
		output << R"({
  "schema_version": 3,
  "profile_id": "msap1-sensor-board-5a",
  "adc_source": "simulator",
  "rms_window_ms": 200,
  "remove_dc": false,
  "adc_reference_volts": 1.0,
  "simulator": {
    "frequency_hz": 60.0,
    "channels": [
      {"channel":0,"rms":5.0,"phase_degrees":0.0},
      {"channel":1,"rms":5.0,"phase_degrees":-120.0},
      {"channel":2,"rms":5.0,"phase_degrees":120.0},
      {"channel":3,"rms":0.0,"phase_degrees":0.0},
      {"channel":4,"rms":120.0,"phase_degrees":120.0},
      {"channel":5,"rms":120.0,"phase_degrees":-120.0},
      {"channel":6,"rms":120.0,"phase_degrees":0.0}
    ]
  },
  "current_channels": [
    {"channel":0,"name":"ILA","enabled":true,"adc_pga_gain":2,
     "sensor_model":"internal_ct","primary_rated_amps":5.0,
     "secondary_rated_amps":0.0025,"burden_ohms":48.71794871794872},
    {"channel":1,"name":"ILB","enabled":true,"adc_pga_gain":2,
     "sensor_model":"internal_ct","primary_rated_amps":5.0,
     "secondary_rated_amps":0.0025,"burden_ohms":48.71794871794872},
    {"channel":2,"name":"ILC","enabled":true,"adc_pga_gain":2,
     "sensor_model":"internal_ct","primary_rated_amps":5.0,
     "secondary_rated_amps":0.0025,"burden_ohms":48.71794871794872},
    {"channel":3,"name":"ILN","enabled":true,"adc_pga_gain":2,
     "sensor_model":"internal_ct","primary_rated_amps":5.0,
     "secondary_rated_amps":0.0025,"burden_ohms":48.71794871794872}
  ],
  "voltage_channels": [
    {"channel":4,"name":"VLC","enabled":true,"adc_pga_gain":1,
     "rin_ohms":6000000.0,"rf_ohms":4640.0},
    {"channel":5,"name":"VLB","enabled":true,"adc_pga_gain":1,
     "rin_ohms":6000000.0,"rf_ohms":4640.0},
    {"channel":6,"name":"VLA","enabled":true,"adc_pga_gain":1,
     "rin_ohms":6000000.0,"rf_ohms":4640.0}
  ]
})";
	}
	const auto configuration = msap1::load_meter_configuration(path, 32000);
	std::filesystem::remove(path);
	/* Fallback window derives from the nominal frequency: one nominal
	 * basic block (12 cycles @ 60 Hz) at 32 kSPS is 6400 samples. */
	require(configuration.wire.rms_window_samples == 6400,
		"wrong nominal basic-block fallback window");
	require(configuration.wire.nominal_frequency_hz == 60,
		"default nominal frequency was not encoded on the wire");
	require(configuration.wire.valid_mask == 0x7f,
		"wrong meter valid mask");
	require((configuration.wire.flags & MSAP1_METER_CONFIG_ENABLE) != 0u,
		"meter configuration is not enabled");
	require((configuration.wire.flags & MSAP1_METER_CONFIG_REMOVE_DC) == 0u,
		"DC-offset removal was not disabled");
	require(configuration.wire.scale_micro_units_q16[0] == 160362,
		"wrong nominal 5 A current coefficient");
	require(configuration.wire.scale_micro_units_q16[4] == 10102371,
		"wrong nominal voltage coefficient");
	require(configuration.wire.adc_pga_gain[0] == 2 &&
		configuration.wire.adc_pga_gain[4] == 1 &&
		configuration.wire.adc_pga_gain[7] == 1,
		"wrong per-channel PGA configuration");
	require(configuration.wire.adc_source == MSAP1_ADC_SOURCE_SIMULATOR &&
		configuration.wire.simulator_frequency_millihz == 60000 &&
		configuration.wire.simulator_valid_mask == 0x7f,
		"wrong simulator source configuration");
	require(configuration.wire.simulator_peak_counts[0] > 0 &&
		configuration.wire.simulator_peak_counts[3] == 0 &&
		configuration.wire.simulator_peak_counts[4] > 0 &&
		configuration.wire.simulator_phase_q32[0] == 0 &&
		configuration.wire.simulator_phase_q32[6] == 0,
		"wrong simulator amplitude or phase conversion");
	require(configuration.wire.frequency_mode ==
			MSAP1_FREQUENCY_MODE_ROLLING_CYCLES &&
		configuration.wire.frequency_reference_channel == 6 &&
		configuration.wire.frequency_averaging_cycles == 10 &&
		configuration.wire.frequency_window_samples == 32000 &&
		configuration.wire.frequency_minimum_millihz == 40000 &&
		configuration.wire.frequency_maximum_millihz == 70000 &&
		configuration.wire.frequency_hysteresis_microvolts == 1000000,
		"frequency configuration is incorrect");
	require(configuration.wire.generation != 0,
		"configuration generation must be non-zero");
	/*
	 * Nominal frequency drives the wire field, the fallback window, and
	 * the generation fingerprint. At 32 kSPS both nominals produce 6400
	 * samples (32000*10/50 and 32000*12/60) — a deliberate coincidence,
	 * not an equivalence.
	 */
	auto nominal_50 = configuration.source;
	nominal_50.nominal_frequency_hz = 50;
	const auto fifty = msap1::prepare_meter_configuration(nominal_50, 32000);
	require(fifty.wire.nominal_frequency_hz == 50 &&
		fifty.wire.rms_window_samples == 6400,
		"50 Hz nominal was rejected or produced a wrong window");
	require(fifty.wire.generation != configuration.wire.generation,
		"nominal frequency change did not change the generation");
	auto nominal_invalid = configuration.source;
	nominal_invalid.nominal_frequency_hz = 55;
	require_throws(
		[&] {
			(void)msap1::prepare_meter_configuration(
				nominal_invalid, 32000);
		},
		"nominal frequency 55 Hz was accepted");

	const auto half_rate = msap1::prepare_meter_configuration(
		configuration.source, 16000);
	require(half_rate.wire.sample_rate_hz == 16000 &&
		half_rate.wire.rms_window_samples == 3200 &&
		half_rate.wire.frequency_window_samples == 16000,
		"runtime sample rate did not update meter windows");
	require(half_rate.wire.generation != configuration.wire.generation,
		"runtime sample rate did not change configuration generation");
	require(msap1::supported_adc_sample_rate(1000) &&
		msap1::supported_adc_sample_rate(128000) &&
		!msap1::supported_adc_sample_rate(19200),
		"supported ADC sample-rate set is incorrect");
	require_throws(
		[&] {
			(void)msap1::prepare_meter_configuration(
				configuration.source, 19200);
		},
		"unsupported fractional ADC profile was accepted");

	const auto active_path = std::filesystem::temp_directory_path() /
		("msap1-meter-active-" + std::to_string(::getpid()) + ".json");
	msap1::save_meter_configuration(configuration.source, active_path);
	const auto reloaded = msap1::load_meter_configuration(active_path, 32000);
	std::filesystem::remove(active_path);
	require(reloaded.wire.generation == configuration.wire.generation,
		"persisted complete profile changed its generation");

	auto invalid_frequency = configuration.source;
	invalid_frequency.frequency.reference_channel = 5;
	require_throws(
		[&] {
			(void)msap1::prepare_meter_configuration(
				invalid_frequency, 32000);
		},
		"non-VLA frequency reference was accepted");
	invalid_frequency = configuration.source;
	invalid_frequency.frequency.minimum_hz = 70.0;
	invalid_frequency.frequency.maximum_hz = 40.0;
	require_throws(
		[&] {
			(void)msap1::prepare_meter_configuration(
				invalid_frequency, 32000);
		},
		"reversed frequency limits were accepted");
	auto extended_frequency = configuration.source;
	extended_frequency.frequency.maximum_hz = 200.0;
	const auto extended_configuration =
		msap1::prepare_meter_configuration(extended_frequency, 32000);
	require(
		extended_configuration.wire.frequency_maximum_millihz == 200000,
		"200 Hz frequency ceiling was not accepted");
	extended_frequency.frequency.maximum_hz = 200.001;
	require_throws(
		[&] {
			(void)msap1::prepare_meter_configuration(
				extended_frequency, 32000);
		},
		"frequency limit above 200 Hz was accepted");
}

void disabled_mv_configuration()
{
	const auto path = std::filesystem::temp_directory_path() /
		("msap1-meter-mv-config-" + std::to_string(::getpid()) + ".json");
	{
		std::ofstream output(path);
		output << R"({
  "schema_version": 2,
  "profile_id": "msap1-sensor-board-mv",
  "rms_window_ms": 200,
  "remove_dc": true,
  "adc_reference_volts": 1.0,
  "current_channels": [
    {"channel":0,"name":"ILA","enabled":false,"adc_pga_gain":1,
     "sensor_model":"voltage_output_current_sensor",
     "rated_output_millivolts":333.0,"frontend_gain":1.0},
    {"channel":1,"name":"ILB","enabled":false,"adc_pga_gain":1,
     "sensor_model":"voltage_output_current_sensor",
     "rated_output_millivolts":333.0,"frontend_gain":1.0},
    {"channel":2,"name":"ILC","enabled":false,"adc_pga_gain":1,
     "sensor_model":"voltage_output_current_sensor",
     "rated_output_millivolts":333.0,"frontend_gain":1.0},
    {"channel":3,"name":"ILN","enabled":false,"adc_pga_gain":1,
     "sensor_model":"voltage_output_current_sensor",
     "rated_output_millivolts":333.0,"frontend_gain":1.0}
  ],
  "voltage_channels": [
    {"channel":4,"name":"VLC","enabled":true,"adc_pga_gain":1,
     "rin_ohms":6000000.0,"rf_ohms":4640.0},
    {"channel":5,"name":"VLB","enabled":true,"adc_pga_gain":1,
     "rin_ohms":6000000.0,"rf_ohms":4640.0},
    {"channel":6,"name":"VLA","enabled":true,"adc_pga_gain":1,
     "rin_ohms":6000000.0,"rf_ohms":4640.0}
  ]
})";
	}
	const auto configuration = msap1::load_meter_configuration(path, 32000);
	std::filesystem::remove(path);
	require(configuration.wire.valid_mask == 0x70,
		"disabled mV current channels became valid");
	for (std::size_t channel = 0; channel < 4; ++channel)
		require(configuration.wire.scale_micro_units_q16[channel] == 0,
			"disabled mV channel has a coefficient");
}

void one_amp_configuration()
{
	const auto path = std::filesystem::temp_directory_path() /
		("msap1-meter-1a-config-" + std::to_string(::getpid()) + ".json");
	{
		std::ofstream output(path);
		output << R"({
  "schema_version":2,
  "profile_id":"msap1-sensor-board-1a",
  "rms_window_ms":200,
  "remove_dc":true,
  "adc_reference_volts":1.0,
  "current_channels":[
    {"channel":0,"name":"ILA","enabled":true,"adc_pga_gain":8,"sensor_model":"internal_ct","primary_rated_amps":5.0,"secondary_rated_amps":0.0025,"burden_ohms":48.71794871794872},
    {"channel":1,"name":"ILB","enabled":true,"adc_pga_gain":8,"sensor_model":"internal_ct","primary_rated_amps":5.0,"secondary_rated_amps":0.0025,"burden_ohms":48.71794871794872},
    {"channel":2,"name":"ILC","enabled":true,"adc_pga_gain":8,"sensor_model":"internal_ct","primary_rated_amps":5.0,"secondary_rated_amps":0.0025,"burden_ohms":48.71794871794872},
    {"channel":3,"name":"ILN","enabled":true,"adc_pga_gain":8,"sensor_model":"internal_ct","primary_rated_amps":5.0,"secondary_rated_amps":0.0025,"burden_ohms":48.71794871794872}
  ],
  "voltage_channels":[
    {"channel":4,"name":"VLC","enabled":true,"adc_pga_gain":1,"rin_ohms":6000000.0,"rf_ohms":4640.0},
    {"channel":5,"name":"VLB","enabled":true,"adc_pga_gain":1,"rin_ohms":6000000.0,"rf_ohms":4640.0},
    {"channel":6,"name":"VLA","enabled":true,"adc_pga_gain":1,"rin_ohms":6000000.0,"rf_ohms":4640.0}
  ]
})";
	}
	const auto configuration = msap1::load_meter_configuration(path, 32000);
	std::filesystem::remove(path);
	require(configuration.wire.scale_micro_units_q16[0] == 40090,
		"wrong nominal 1 A current coefficient");
	for (std::size_t channel = 0; channel < 4; ++channel)
		require(configuration.wire.adc_pga_gain[channel] == 8,
			"wrong 1 A profile PGA");
}

void rejects_incomplete_configuration()
{
	const auto path = std::filesystem::temp_directory_path() /
		("msap1-meter-incomplete-" + std::to_string(::getpid()) + ".json");
	{
		std::ofstream output(path);
		output << R"({
  "schema_version":2,
  "profile_id":"incomplete",
  "rms_window_ms":200,
  "remove_dc":true,
  "adc_reference_volts":1.0,
  "current_channels":[
    {"channel":0,"name":"ILA","enabled":true,"adc_pga_gain":2,"sensor_model":"internal_ct","primary_rated_amps":5.0,"secondary_rated_amps":0.0025,"burden_ohms":48.71794871794872}
  ],
  "voltage_channels":[
    {"channel":4,"name":"VLC","enabled":true,"adc_pga_gain":1,"rin_ohms":6000000.0,"rf_ohms":4640.0}
  ]
})";
	}
	require_throws(
		[&] { (void)msap1::load_meter_configuration(path, 32000); },
		"incomplete profile was accepted");
	std::filesystem::remove(path);
}

void rejects_bad_frames()
{
	std::vector<std::uint8_t> short_frame(4);
	require_throws(
		[&] { msap1::decode_message(short_frame.data(), short_frame.size()); },
		"short frame was accepted");
	auto wire = msap1::encode_request(MSAP1_RPU_MSG_PING, 1);
	auto *header = reinterpret_cast<msap1_rpu_msg_header *>(wire.data());
	header->version = 1;
	require_throws([&] { msap1::decode_message(wire.data(), wire.size()); },
		"old protocol version was accepted");
}

void meter_health_evaluation()
{
	msap1::InfoResponse response{};
	msap1_adc_health_payload rpu_health{};
	response.running = true;
	response.has_meter_record = true;
	response.meter_record_age_ms = 0;
	response.configuration_generation = 0x1234;
	/* Historical rejections remain observable but must not poison a clean
	 * capture epoch forever. */
	response.lifetime_invalid_records = 58;
	rpu_health.health_flags =
		MSAP1_ADC_HEALTH_SPI_RESPONSIVE | MSAP1_ADC_HEALTH_INITIALIZED |
		MSAP1_ADC_HEALTH_INIT_COMPLETE | MSAP1_ADC_HEALTH_CONFIG_MATCH |
		MSAP1_ADC_HEALTH_CAPTURE_ACTIVE | MSAP1_ADC_HEALTH_NO_OVERFLOW |
		MSAP1_ADC_HEALTH_HEADERS_VALID | MSAP1_ADC_HEALTH_RATE_MATCH;
	rpu_health.meter_health_flags =
		MSAP1_METER_HEALTH_CORES_PRESENT | MSAP1_METER_HEALTH_CONFIGURED |
		MSAP1_METER_HEALTH_GENERATION_MATCH | MSAP1_METER_HEALTH_ENABLED |
		MSAP1_METER_HEALTH_REMOVE_DC;
	rpu_health.meter_generation = response.configuration_generation;
	response.rpu_health = rpu_health;
	const auto healthy = msap1::evaluate_meter_health(response);
	require(healthy.healthy && healthy.acquisition_healthy && healthy.adc_healthy,
		"healthy meter response was rejected");
	require(healthy.frequency_arithmetic_ok,
		"healthy frequency arithmetic was rejected");
	require(healthy.dc_offset_removal,
		"DC-offset removal health flag was not exposed");
	require(healthy.rate_match,
		"ADC rate-match health flag was not exposed");
	require(healthy.adc_degraded_reasons.empty(),
		"healthy ADC response reported degradation reasons");

	response.invalid_records = 1;
	const auto current_rejection = msap1::evaluate_meter_health(response);
	require(!current_rejection.healthy &&
			!current_rejection.acquisition_healthy,
		"current-epoch rejection did not degrade acquisition health");
	response.invalid_records = 0;

	msap1_aggregation_health_payload aggregation{};
	aggregation.health_flags =
		MSAP1_AGGREGATION_HEALTH_TRANSPORT_AVAILABLE |
		MSAP1_AGGREGATION_HEALTH_TRANSPORT_INITIALIZED |
		MSAP1_AGGREGATION_HEALTH_INPUT_HEALTHY |
		MSAP1_AGGREGATION_HEALTH_ENGINE_READY |
		MSAP1_AGGREGATION_HEALTH_OUTPUT_READY |
		MSAP1_AGGREGATION_HEALTH_OUTPUT_ACTIVE |
		MSAP1_AGGREGATION_HEALTH_AUTHORITATIVE;
	aggregation.software_ring_capacity = 64;
	response.has_aggregation_health = true;
	response.rpu_aggregation_health = aggregation;
	const auto aggregation_healthy = msap1::evaluate_meter_health(response);
	require(aggregation_healthy.healthy &&
			aggregation_healthy.aggregation_healthy &&
			aggregation_healthy.aggregation_authoritative,
		"healthy authoritative aggregation response was rejected");

	aggregation.software_ring_push_failures = 1;
	aggregation.input_records_dropped = 2;
	aggregation.first_dropped_sequence = 100;
	aggregation.last_dropped_sequence = 101;
	aggregation.software_ring_current = 60;
	aggregation.software_ring_pressure =
		MSAP1_AGGREGATION_RING_PRESSURE_CRITICAL;
	const auto aggregation_reasons =
		msap1::evaluate_rpu_aggregation_health_reasons(aggregation);
	auto has_aggregation_reason = [&](const char *code) {
		for (const auto &reason : aggregation_reasons)
			if (reason.code == code)
				return true;
		return false;
	};
	require(has_aggregation_reason("input_ring_push_failures"),
		"software-ring push failure was not diagnosed");
	require(has_aggregation_reason("input_records_dropped"),
		"deterministic input loss was not diagnosed");
	require(has_aggregation_reason("input_ring_pressure_critical"),
		"critical ring pressure was not diagnosed");
	response.has_aggregation_health = false;
	response.rpu_aggregation_health = msap1_aggregation_health_payload{};

	response.meter_record_age_ms =
		msap1::meter_record_stale_after_ms + 1u;
	const auto stale = msap1::evaluate_meter_health(response);
	require(!stale.healthy && !stale.acquisition_healthy &&
			stale.record_stale && stale.adc_healthy,
		"stale meter data did not degrade acquisition health");
	response.meter_record_age_ms = 0;

	response.sequence_gaps = 1;
	const auto degraded = msap1::evaluate_meter_health(response);
	require(!degraded.healthy && !degraded.acquisition_healthy &&
		degraded.adc_healthy,
		"acquisition errors did not degrade meter health");

	response.sequence_gaps = 0;
	rpu_health.health_flags &= ~MSAP1_ADC_HEALTH_RATE_MATCH;
	response.rpu_health = rpu_health;
	const auto rate_mismatch = msap1::evaluate_meter_health(response);
	require(!rate_mismatch.healthy && !rate_mismatch.adc_healthy &&
		rate_mismatch.acquisition_healthy,
		"ADC rate mismatch did not degrade meter health");
	require(rate_mismatch.adc_degraded_reasons.size() == 1 &&
			rate_mismatch.adc_degraded_reasons.front().code ==
				"sample_rate_mismatch",
		"ADC rate mismatch did not expose its degradation reason");
	require(rate_mismatch.adc_degraded_reasons.front().message.find(
			"does not match configured rate") != std::string::npos,
		"ADC rate-mismatch reason omitted measured/configured context");

	rpu_health.health_flags |= MSAP1_ADC_HEALTH_RATE_MATCH;
	rpu_health.health_flags &=
		~(MSAP1_ADC_HEALTH_SPI_RESPONSIVE |
		  MSAP1_ADC_HEALTH_INIT_COMPLETE |
		  MSAP1_ADC_HEALTH_CONFIG_MATCH);
	rpu_health.spi_error = MSAP1_ADC_SPI_HEALTH_PROTOCOL_FAILED;
	response.rpu_health = rpu_health;
	const auto spi_failure = msap1::evaluate_meter_health(response);
	require(spi_failure.adc_degraded_reasons.size() == 1 &&
			spi_failure.adc_degraded_reasons.front().code ==
				"spi_unresponsive",
		"invalid SPI snapshot produced secondary register-health reasons");
	rpu_health.health_flags |=
		MSAP1_ADC_HEALTH_SPI_RESPONSIVE |
		MSAP1_ADC_HEALTH_INIT_COMPLETE |
		MSAP1_ADC_HEALTH_CONFIG_MATCH;
	rpu_health.spi_error = MSAP1_ADC_SPI_HEALTH_OK;
	response.rpu_health = rpu_health;

	response.latest_record.words[57] = 1u << 7;
	const auto arithmetic_fault = msap1::evaluate_meter_health(response);
	require(!arithmetic_fault.healthy &&
		!arithmetic_fault.frequency_arithmetic_ok,
		"frequency arithmetic error did not degrade meter health");
}

} // namespace

int main()
{
	try {
		request_round_trip();
		meter_ack_round_trip();
		adc_health_round_trip();
		aggregation_health_round_trip();
		adc_diagnostic_round_trip();
		meter_config_wire_layout();
		simulator_event_wire_layout();
		meter_record_contract();
		meter_configuration();
		disabled_mv_configuration();
		one_amp_configuration();
		rejects_incomplete_configuration();
		rejects_bad_frames();
		meter_health_evaluation();
		std::cout << "protocol tests passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "protocol test failed: " << error.what() << '\n';
		return 1;
	}
}
