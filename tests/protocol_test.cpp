#include "msap1/meter_config.hpp"
#include "msap1/meter_health.hpp"
#include "msap1/meter_record.hpp"
#include "msap1/protocol.hpp"

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
}

void meter_configuration()
{
	const auto path = std::filesystem::temp_directory_path() /
		("msap1-meter-config-" + std::to_string(::getpid()) + ".json");
	{
		std::ofstream output(path);
		output << R"({
  "schema_version": 2,
  "profile_id": "acuvim3-sb-5a",
  "rms_window_ms": 200,
  "remove_dc": false,
  "adc_reference_volts": 1.0,
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
	require(configuration.wire.rms_window_samples == 6400,
		"wrong 200 ms RMS window");
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
	require(configuration.wire.frequency_mode ==
			MSAP1_FREQUENCY_MODE_ROLLING_CYCLES &&
		configuration.wire.frequency_reference_channel == 6 &&
		configuration.wire.frequency_averaging_cycles == 10 &&
		configuration.wire.frequency_window_samples == 32000 &&
		configuration.wire.frequency_minimum_millihz == 40000 &&
		configuration.wire.frequency_maximum_millihz == 70000 &&
		configuration.wire.frequency_hysteresis_microvolts == 1000000,
		"schema-v2 frequency defaults are incorrect");
	require(configuration.wire.generation != 0,
		"configuration generation must be non-zero");
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
  "profile_id": "acuvim3-sb-mv",
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
  "profile_id":"acuvim3-sb-1a",
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
	msap1::AcquisitionResponse response{};
	response.running = 1;
	response.has_meter_record = 1;
	response.configuration_generation = 0x1234;
	response.rpu_health.health_flags =
		MSAP1_ADC_HEALTH_SPI_RESPONSIVE | MSAP1_ADC_HEALTH_INITIALIZED |
		MSAP1_ADC_HEALTH_INIT_COMPLETE | MSAP1_ADC_HEALTH_CONFIG_MATCH |
		MSAP1_ADC_HEALTH_CAPTURE_ACTIVE | MSAP1_ADC_HEALTH_NO_OVERFLOW |
		MSAP1_ADC_HEALTH_HEADERS_VALID;
	response.rpu_health.meter_health_flags =
		MSAP1_METER_HEALTH_CORES_PRESENT | MSAP1_METER_HEALTH_CONFIGURED |
		MSAP1_METER_HEALTH_GENERATION_MATCH | MSAP1_METER_HEALTH_ENABLED |
		MSAP1_METER_HEALTH_REMOVE_DC;
	response.rpu_health.meter_generation = response.configuration_generation;
	const auto healthy = msap1::evaluate_meter_health(response);
	require(healthy.healthy && healthy.acquisition_healthy && healthy.adc_healthy,
		"healthy meter response was rejected");
	require(healthy.frequency_arithmetic_ok,
		"healthy frequency arithmetic was rejected");
	require(healthy.dc_offset_removal,
		"DC-offset removal health flag was not exposed");

	response.sequence_gaps = 1;
	const auto degraded = msap1::evaluate_meter_health(response);
	require(!degraded.healthy && !degraded.acquisition_healthy &&
		degraded.adc_healthy,
		"acquisition errors did not degrade meter health");

	response.sequence_gaps = 0;
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
		adc_diagnostic_round_trip();
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
