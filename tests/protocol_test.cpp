#include "msap1/protocol.hpp"
#include "msap1/shared_ring.hpp"
#include "msap1/visualizer.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <sys/mman.h>
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
	health.src_n_lsb = 64;

	const auto wire = msap1::encode_request(MSAP1_RPU_MSG_ADC_HEALTH,
						23, &health, sizeof(health));
	const auto message = msap1::decode_message(wire.data(), wire.size());
	const auto decoded = msap1::decode_adc_health(message);
	require(decoded.sample_rate_hz == 32000, "wrong health sample rate");
	require(decoded.frame_count == 123456, "wrong health frame count");
	require(decoded.header_error_count == 12,
		"wrong health header error count");
	require(decoded.expected_decimation == 64, "wrong expected decimation");
}

void shared_ring_multi_reader()
{
	const std::string name = "/msap1-ring-test-" + std::to_string(::getpid());
	::shm_unlink(name.c_str());
	{
		msap1::SharedRingWriter writer(name, 4);
		msap1::SharedRingReader first(name);
		msap1::SharedRingReader second(name);
		msap1::AdcSampleFrame frames[6]{};
		for (std::size_t index = 0; index < 6; ++index)
			frames[index][0] = static_cast<std::int32_t>(index);
		writer.publish(frames, 2);

		std::uint64_t first_cursor = 0;
		std::uint64_t second_cursor = 0;
		std::uint64_t first_dropped = 0;
		std::uint64_t second_dropped = 0;
		msap1::AdcSampleFrame frame{};
		require(first.read(first_cursor, frame, first_dropped),
			"first reader did not receive data");
		require(frame[0] == 0, "first reader saw wrong frame");
		require(second.read(second_cursor, frame, second_dropped),
			"second reader did not receive data");
		require(frame[0] == 0, "second reader was affected by first reader");

		writer.publish(frames + 2, 4);
		first_cursor = 0;
		require(first.read(first_cursor, frame, first_dropped),
			"overrun reader did not recover");
		require(first_dropped == 2 && frame[0] == 2,
			"overrun reader did not report skipped frames");
	}
	::shm_unlink(name.c_str());
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
	require(text.find("ch6") != std::string::npos, "channel 6 missing");
	require(text.find("ch0") == std::string::npos, "channel 0 was not filtered");
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

} // namespace

int main()
{
	try {
		request_round_trip();
		adc_health_round_trip();
		shared_ring_multi_reader();
		voltage_channel_filter();
		rejects_bad_frames();
		std::cout << "protocol tests passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "protocol test failed: " << error.what() << '\n';
		return 1;
	}
}
