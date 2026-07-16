#include "msap1/protocol.hpp"
#include "msap1/visualizer.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

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
	msap1_adc_stream_request request{};
	request.display_rate_hz = 1000;
	const auto wire = msap1::encode_request(MSAP1_RPU_MSG_ADC_STREAM_START,
						5, &request, sizeof(request));
	const auto decoded = msap1::decode_message(wire.data(), wire.size());
	require(decoded.header.type == MSAP1_RPU_MSG_ADC_STREAM_START,
		"wrong message type");
	require(decoded.header.sequence == 5, "wrong message sequence");
	require(decoded.payload.size() == sizeof(request), "wrong payload size");
	msap1_adc_stream_request decoded_request{};
	std::memcpy(&decoded_request, decoded.payload.data(), sizeof(decoded_request));
	require(decoded_request.display_rate_hz == 1000, "wrong display rate");
}

void sample_batch_round_trip()
{
	msap1_adc_sample_batch batch{};
	batch.adc_sample_rate_hz = 32000;
	batch.display_rate_hz = 1000;
	batch.first_frame_index = 64000;
	batch.capture_flags = 0x12;
	batch.frame_count = 2;
	batch.channel_count = MSAP1_ADC_CHANNEL_COUNT;
	for (unsigned frame = 0; frame < batch.frame_count; ++frame)
		for (unsigned channel = 0; channel < MSAP1_ADC_CHANNEL_COUNT; ++channel)
			batch.frames[frame].channel[channel] =
				static_cast<std::int32_t>(frame * 100 + channel);

	const auto payload_size = MSAP1_ADC_SAMPLE_BATCH_HEADER_SIZE +
		batch.frame_count * sizeof(msap1_adc_sample_frame);
	const auto wire = msap1::encode_request(MSAP1_RPU_MSG_ADC_SAMPLE_BATCH,
						17, &batch, payload_size);
	const auto message = msap1::decode_message(wire.data(), wire.size());
	const auto decoded = msap1::decode_adc_batch(message);
	require(decoded.adc_sample_rate_hz == 32000, "wrong ADC sample rate");
	require(decoded.display_rate_hz == 1000, "wrong display sample rate");
	require(decoded.first_frame_index == 64000, "wrong first frame index");
	require(decoded.capture_flags == 0x12, "wrong capture flags");
	require(decoded.frames.size() == 2, "wrong frame count");
	require(decoded.frames[1][7] == 107, "wrong channel sample");
}

void adc_health_round_trip()
{
	msap1_adc_health_payload health{};
	health.health_flags = MSAP1_ADC_HEALTH_SPI_RESPONSIVE |
		MSAP1_ADC_HEALTH_INITIALIZED | MSAP1_ADC_HEALTH_CONFIG_MATCH;
	health.sample_rate_hz = 32000;
	health.frame_count = 123456;
	health.header_error_count = 12;
	health.expected_decimation = 64;
	health.status_3 = 0x10;
	health.dout_format = 0;
	health.src_n_lsb = 64;

	const auto wire = msap1::encode_request(MSAP1_RPU_MSG_ADC_HEALTH,
						23, &health, sizeof(health));
	const auto message = msap1::decode_message(wire.data(), wire.size());
	const auto decoded = msap1::decode_adc_health(message);
	require(decoded.sample_rate_hz == 32000, "wrong health sample rate");
	require(decoded.frame_count == 123456, "wrong health frame count");
	require(decoded.header_error_count == 12,
		"wrong health header error count");
	require(decoded.expected_decimation == 64,
		"wrong expected decimation");
	require(decoded.status_3 == 0x10, "wrong STATUS_3 value");
}

void voltage_channel_filter()
{
	msap1::AdcBatch batch;
	batch.adc_sample_rate_hz = 32000;
	batch.display_rate_hz = 1000;
	batch.frames.push_back({0, 1, 2, 3, 400, 500, 600, 7});
	std::ostringstream output;
	msap1::Visualizer visualizer(output, msap1::OutputFormat::table,
				      {4, 5, 6});
	visualizer.render(batch);
	const auto text = output.str();
	require(text.find("ch4") != std::string::npos, "channel 4 missing");
	require(text.find("ch5") != std::string::npos, "channel 5 missing");
	require(text.find("ch6") != std::string::npos, "channel 6 missing");
	require(text.find("ch0") == std::string::npos, "channel 0 was not filtered");
	require(text.find("400") != std::string::npos, "channel 4 sample missing");
	require(text.find("600") != std::string::npos, "channel 6 sample missing");
}

void rejects_bad_frames()
{
	std::vector<std::uint8_t> short_frame(4);
	require_throws(
		[&] { msap1::decode_message(short_frame.data(), short_frame.size()); },
		"short frame was accepted");

	msap1_adc_sample_batch batch{};
	batch.adc_sample_rate_hz = 32000;
	batch.display_rate_hz = 3000;
	batch.frame_count = 1;
	batch.channel_count = MSAP1_ADC_CHANNEL_COUNT;
	const auto payload_size = MSAP1_ADC_SAMPLE_BATCH_HEADER_SIZE +
		sizeof(msap1_adc_sample_frame);
	const auto wire = msap1::encode_request(MSAP1_RPU_MSG_ADC_SAMPLE_BATCH,
						1, &batch, payload_size);
	const auto message = msap1::decode_message(wire.data(), wire.size());
	require_throws([&] { msap1::decode_adc_batch(message); },
		       "non-divisor display rate was accepted");
}

} // namespace

int main()
{
	try {
		request_round_trip();
		sample_batch_round_trip();
		adc_health_round_trip();
		voltage_channel_filter();
		rejects_bad_frames();
		std::cout << "protocol tests passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "protocol test failed: " << error.what() << '\n';
		return 1;
	}
}
