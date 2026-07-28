#include "msap1/protocol.hpp"

#include <cstddef>
#include <cstring>
#include <stdexcept>

namespace msap1 {

static_assert(sizeof(msap1_rpu_msg_header) == 16,
	      "unexpected RPMsg header layout");
static_assert(sizeof(msap1_adc_health_payload) == 174,
	      "unexpected ADC health payload layout");
static_assert(sizeof(msap1_meter_config_payload) == 92,
	      "unexpected meter configuration payload layout");
static_assert(sizeof(msap1_meter_config_ack_payload) == 20,
	      "unexpected meter configuration acknowledgement layout");
static_assert(sizeof(msap1_adc_diagnostic_payload) == 188,
	      "unexpected ADC diagnostic payload layout");
static_assert(sizeof(msap1_rpu_msg_header) +
	      sizeof(msap1_adc_health_payload) <= MSAP1_RPU_MAX_FRAME_SIZE,
	      "ADC health response exceeds RPMsg protocol frame");
static_assert(sizeof(msap1_rpu_msg_header) +
	      sizeof(msap1_adc_diagnostic_payload) <=
		      MSAP1_RPU_MAX_FRAME_SIZE,
	      "ADC diagnostic response exceeds RPMsg protocol frame");

std::vector<std::uint8_t> encode_request(std::uint8_t type,
					std::uint32_t sequence,
					const void *payload,
					std::size_t payload_size)
{
	if (payload_size > MSAP1_RPU_MAX_FRAME_SIZE -
				   sizeof(msap1_rpu_msg_header) ||
	    (payload_size != 0 && payload == nullptr))
		throw std::invalid_argument("invalid RPMsg request payload");

	msap1_rpu_msg_header header{};
	header.magic = MSAP1_RPU_MAGIC;
	header.version = MSAP1_RPU_VERSION;
	header.type = type;
	header.payload_len = static_cast<std::uint16_t>(payload_size);
	header.sequence = sequence;
	header.status = MSAP1_RPU_STATUS_OK;

	std::vector<std::uint8_t> frame(sizeof(header) + payload_size);
	std::memcpy(frame.data(), &header, sizeof(header));
	if (payload_size != 0)
		std::memcpy(frame.data() + sizeof(header), payload, payload_size);
	return frame;
}

Message decode_message(const void *data, std::size_t size)
{
	if (data == nullptr || size < sizeof(msap1_rpu_msg_header) ||
	    size > MSAP1_RPU_MAX_FRAME_SIZE)
		throw std::runtime_error("invalid RPMsg frame length");

	Message message;
	std::memcpy(&message.header, data, sizeof(message.header));
	if (message.header.magic != MSAP1_RPU_MAGIC)
		throw std::runtime_error("invalid RPMsg magic");
	if (message.header.version != MSAP1_RPU_VERSION)
		throw std::runtime_error("unsupported RPMsg protocol version");
	if (sizeof(message.header) + message.header.payload_len != size)
		throw std::runtime_error("RPMsg payload length mismatch");

	const auto *bytes = static_cast<const std::uint8_t *>(data);
	message.payload.assign(bytes + sizeof(message.header), bytes + size);
	return message;
}

msap1_adc_health_payload decode_adc_health(const Message &message)
{
	if (message.header.type != MSAP1_RPU_MSG_ADC_HEALTH ||
	    message.header.status != MSAP1_RPU_STATUS_OK ||
	    message.payload.size() != sizeof(msap1_adc_health_payload))
		throw std::runtime_error("message is not an ADC health response");

	msap1_adc_health_payload health{};
	std::memcpy(&health, message.payload.data(), sizeof(health));
	return health;
}

msap1_adc_diagnostic_payload decode_adc_diagnostic(const Message &message)
{
	if (message.header.type != MSAP1_RPU_MSG_ADC_DIAGNOSTIC ||
	    message.header.status != MSAP1_RPU_STATUS_OK ||
	    message.payload.size() != sizeof(msap1_adc_diagnostic_payload))
		throw std::runtime_error(
			"message is not an ADC diagnostic response");

	msap1_adc_diagnostic_payload diagnostic{};
	std::memcpy(&diagnostic, message.payload.data(), sizeof(diagnostic));
	return diagnostic;
}

msap1_meter_config_ack_payload decode_meter_config_ack(const Message &message)
{
	if (message.header.type != MSAP1_RPU_MSG_ACK ||
	    message.header.status != MSAP1_RPU_STATUS_OK ||
	    message.payload.size() != sizeof(msap1_meter_config_ack_payload))
		throw std::runtime_error(
			"message is not a meter configuration acknowledgement");

	msap1_meter_config_ack_payload acknowledgement{};
	std::memcpy(&acknowledgement, message.payload.data(),
		    sizeof(acknowledgement));
	return acknowledgement;
}

} // namespace msap1
