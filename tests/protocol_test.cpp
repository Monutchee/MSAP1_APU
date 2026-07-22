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
	health.expected_decimation = 64;

	const auto wire = msap1::encode_request(MSAP1_RPU_MSG_ADC_HEALTH,
		23, &health, sizeof(health));
	const auto message = msap1::decode_message(wire.data(), wire.size());
	const auto decoded = msap1::decode_adc_health(message);
	require(decoded.sample_rate_hz == 32000, "wrong health sample rate");
	require(decoded.frame_count == 123456, "wrong health frame count");
	require(decoded.meter_generation == 0x11223344,
		"wrong health meter generation");
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
	require(record.header_valid(), "valid meter header rejected");
	require(record.sequence() == 41, "wrong meter sequence");
	const auto channel = record.channel(6);
	require(channel.valid, "voltage channel is not valid");
	require(channel.mean_micro_units == -125000, "wrong signed mean");
	require(channel.rms_count == 777, "wrong RMS count");
	require(channel.rms_micro_units == 230123456, "wrong RMS voltage");
}

void meter_configuration()
{
	const auto path = std::filesystem::temp_directory_path() /
		("msap1-meter-config-" + std::to_string(::getpid()) + ".json");
	{
		std::ofstream output(path);
		output << R"({
  "schema_version": 1,
  "rms_window_ms": 200,
  "remove_dc": false,
  "adc_reference_volts": 1.0,
  "adc_pga_gain": 1.0,
  "voltage_channels": [
    {"channel":4,"name":"VLC","rin_ohms":6000000.0,"rf_ohms":4640.0},
    {"channel":5,"name":"VLB","rin_ohms":6000000.0,"rf_ohms":4640.0},
    {"channel":6,"name":"VLA","rin_ohms":6000000.0,"rf_ohms":4640.0}
  ]
})";
	}
	const auto configuration = msap1::load_meter_configuration(path, 32000);
	std::filesystem::remove(path);
	require(configuration.wire.rms_window_samples == 6400,
		"wrong 200 ms RMS window");
	require(configuration.wire.valid_mask == 0x70,
		"wrong voltage valid mask");
	require((configuration.wire.flags & MSAP1_METER_CONFIG_ENABLE) != 0u,
		"meter configuration is not enabled");
	require((configuration.wire.flags & MSAP1_METER_CONFIG_REMOVE_DC) == 0u,
		"DC-offset removal was not disabled");
	require(configuration.wire.scale_micro_units_q16[4] == 10102371,
		"wrong nominal voltage coefficient");
	require(configuration.wire.generation != 0,
		"configuration generation must be non-zero");
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
	require(healthy.dc_offset_removal,
		"DC-offset removal health flag was not exposed");

	response.sequence_gaps = 1;
	const auto degraded = msap1::evaluate_meter_health(response);
	require(!degraded.healthy && !degraded.acquisition_healthy &&
		degraded.adc_healthy,
		"acquisition errors did not degrade meter health");
}

} // namespace

int main()
{
	try {
		request_round_trip();
		meter_ack_round_trip();
		adc_health_round_trip();
		meter_record_contract();
		meter_configuration();
		rejects_bad_frames();
		meter_health_evaluation();
		std::cout << "protocol tests passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "protocol test failed: " << error.what() << '\n';
		return 1;
	}
}
