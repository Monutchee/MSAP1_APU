#include "msap1/protocol.hpp"

#include <cstddef>
#include <cstring>
#include <stdexcept>

namespace msap1 {

static_assert(sizeof(msap1_rpu_msg_header) == 16,
	      "unexpected RPMsg header layout");
static_assert(sizeof(msap1_adc_sample_frame) == 32,
	      "unexpected ADC frame layout");
static_assert(offsetof(msap1_adc_sample_batch, frames) ==
	      MSAP1_ADC_SAMPLE_BATCH_HEADER_SIZE,
	      "unexpected ADC batch header layout");
static_assert(sizeof(msap1_rpu_msg_header) +
	      sizeof(msap1_adc_sample_batch) <= MSAP1_RPU_MAX_FRAME_SIZE,
	      "ADC batch exceeds RPMsg protocol frame");
static_assert(sizeof(msap1_adc_health_payload) == 48,
	      "unexpected ADC health payload layout");
static_assert(sizeof(msap1_rpu_msg_header) +
	      sizeof(msap1_adc_health_payload) <= MSAP1_RPU_MAX_FRAME_SIZE,
	      "ADC health response exceeds RPMsg protocol frame");

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

AdcBatch decode_adc_batch(const Message &message)
{
	if (message.header.type != MSAP1_RPU_MSG_ADC_SAMPLE_BATCH ||
	    message.payload.size() < MSAP1_ADC_SAMPLE_BATCH_HEADER_SIZE)
		throw std::runtime_error("message is not an ADC sample batch");

	msap1_adc_sample_batch wire{};
	std::memcpy(&wire, message.payload.data(),
		    MSAP1_ADC_SAMPLE_BATCH_HEADER_SIZE);
	if (wire.channel_count != MSAP1_ADC_CHANNEL_COUNT ||
	    wire.frame_count == 0 ||
	    wire.frame_count > MSAP1_ADC_MAX_BATCH_FRAMES ||
	    wire.adc_sample_rate_hz == 0 || wire.display_rate_hz == 0 ||
	    wire.adc_sample_rate_hz % wire.display_rate_hz != 0)
		throw std::runtime_error("invalid ADC batch metadata");

	const std::size_t expected_size = MSAP1_ADC_SAMPLE_BATCH_HEADER_SIZE +
		wire.frame_count * sizeof(msap1_adc_sample_frame);
	if (message.payload.size() != expected_size)
		throw std::runtime_error("invalid ADC batch payload length");

	AdcBatch batch;
	batch.adc_sample_rate_hz = wire.adc_sample_rate_hz;
	batch.display_rate_hz = wire.display_rate_hz;
	batch.first_frame_index = wire.first_frame_index;
	batch.capture_flags = wire.capture_flags;
	batch.frames.resize(wire.frame_count);
	const auto *frame_bytes = message.payload.data() +
		MSAP1_ADC_SAMPLE_BATCH_HEADER_SIZE;
	for (std::size_t i = 0; i < batch.frames.size(); ++i)
		std::memcpy(batch.frames[i].data(),
			    frame_bytes + i * sizeof(msap1_adc_sample_frame),
			    sizeof(msap1_adc_sample_frame));
	return batch;
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

} // namespace msap1
