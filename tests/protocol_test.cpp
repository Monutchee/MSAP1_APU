#include "msap1/protocol.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
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
		rejects_bad_frames();
		std::cout << "protocol tests passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "protocol test failed: " << error.what() << '\n';
		return 1;
	}
}
