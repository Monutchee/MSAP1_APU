#ifndef MSAP1_APU_PROTOCOL_HPP
#define MSAP1_APU_PROTOCOL_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

#include "msap1/acquisition/rpu/rpu_control_protocol.h"

namespace msap1 {

struct Message {
	msap1_rpu_msg_header header{};
	std::vector<std::uint8_t> payload;
};

std::vector<std::uint8_t> encode_request(std::uint8_t type,
					std::uint32_t sequence,
					const void *payload = nullptr,
					std::size_t payload_size = 0);
Message decode_message(const void *data, std::size_t size);
msap1_adc_health_payload decode_adc_health(const Message &message);
msap1_adc_diagnostic_payload decode_adc_diagnostic(const Message &message);
msap1_meter_config_ack_payload decode_meter_config_ack(const Message &message);
msap1_simulator_event_ack_payload decode_simulator_event_ack(
	const Message &message);

} // namespace msap1

#endif
