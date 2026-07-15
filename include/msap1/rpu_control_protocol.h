#ifndef MSAP1_RPU_CONTROL_PROTOCOL_H
#define MSAP1_RPU_CONTROL_PROTOCOL_H

/* Wire ABI shared with MSAP1_RPU/common/include/rpu_control_protocol.h. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MSAP1_RPU_MAGIC 0x4d525055u
#define MSAP1_RPU_VERSION 1u
#define MSAP1_RPU_MAX_FRAME_SIZE 256u
#define MSAP1_ADC_CHANNEL_COUNT 8u
#define MSAP1_ADC_MAX_BATCH_FRAMES 6u
#define MSAP1_ADC_SAMPLE_BATCH_HEADER_SIZE 24u

enum msap1_rpu_msg_type {
	MSAP1_RPU_MSG_PING = 1,
	MSAP1_RPU_MSG_PONG = 2,
	MSAP1_RPU_MSG_GET_STATUS = 3,
	MSAP1_RPU_MSG_STATUS = 4,
	MSAP1_RPU_MSG_SET_LED = 5,
	MSAP1_RPU_MSG_ACK = 6,
	MSAP1_RPU_MSG_ERROR = 7,
	MSAP1_RPU_MSG_ADC_STREAM_START = 8,
	MSAP1_RPU_MSG_ADC_STREAM_STOP = 9,
	MSAP1_RPU_MSG_ADC_SAMPLE_BATCH = 10,
};

enum msap1_rpu_status_code {
	MSAP1_RPU_STATUS_OK = 0,
	MSAP1_RPU_STATUS_BAD_MAGIC = 1,
	MSAP1_RPU_STATUS_BAD_VERSION = 2,
	MSAP1_RPU_STATUS_BAD_LENGTH = 3,
	MSAP1_RPU_STATUS_BAD_TYPE = 4,
	MSAP1_RPU_STATUS_BAD_PAYLOAD = 5,
	MSAP1_RPU_STATUS_INTERNAL_ERROR = 6,
	MSAP1_RPU_STATUS_ADC_UNAVAILABLE = 7,
};

enum msap1_rpu_led_mode {
	MSAP1_RPU_LED_OFF = 0,
	MSAP1_RPU_LED_ON = 1,
	MSAP1_RPU_LED_TOGGLE = 2,
	MSAP1_RPU_LED_HEARTBEAT = 3,
};

struct msap1_rpu_msg_header {
	uint32_t magic;
	uint8_t version;
	uint8_t type;
	uint16_t payload_len;
	uint32_t sequence;
	uint32_t status;
} __attribute__((packed));

struct msap1_rpu_led_payload {
	uint8_t mode;
	uint8_t reserved[3];
} __attribute__((packed));

struct msap1_rpu_status_payload {
	uint32_t core_id;
	uint32_t led_mode;
	uint32_t led_on;
	uint32_t heartbeat_count;
	uint32_t uptime_ms;
	uint32_t rx_count;
	uint32_t error_count;
} __attribute__((packed));

struct msap1_adc_stream_request {
	uint32_t display_rate_hz;
} __attribute__((packed));

struct msap1_adc_sample_frame {
	int32_t channel[MSAP1_ADC_CHANNEL_COUNT];
} __attribute__((packed));

struct msap1_adc_sample_batch {
	uint32_t adc_sample_rate_hz;
	uint32_t display_rate_hz;
	uint64_t first_frame_index;
	uint32_t capture_flags;
	uint16_t frame_count;
	uint16_t channel_count;
	struct msap1_adc_sample_frame frames[MSAP1_ADC_MAX_BATCH_FRAMES];
} __attribute__((packed));

#ifdef __cplusplus
}
#endif

#endif /* MSAP1_RPU_CONTROL_PROTOCOL_H */
