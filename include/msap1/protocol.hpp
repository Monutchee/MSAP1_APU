#ifndef MSAP1_APU_PROTOCOL_HPP
#define MSAP1_APU_PROTOCOL_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "msap1/rpu_control_protocol.h"

namespace msap1 {

struct Message {
	msap1_rpu_msg_header header{};
	std::vector<std::uint8_t> payload;
};

struct AdcBatch {
	std::uint32_t adc_sample_rate_hz = 0;
	std::uint32_t display_rate_hz = 0;
	std::uint64_t first_frame_index = 0;
	std::uint32_t capture_flags = 0;
	std::vector<std::array<std::int32_t, MSAP1_ADC_CHANNEL_COUNT>> frames;
};

std::vector<std::uint8_t> encode_request(std::uint8_t type,
					std::uint32_t sequence,
					const void *payload = nullptr,
					std::size_t payload_size = 0);
Message decode_message(const void *data, std::size_t size);
AdcBatch decode_adc_batch(const Message &message);
msap1_adc_health_payload decode_adc_health(const Message &message);

} // namespace msap1

#endif
