#ifndef MSAP1_RPU_CONTROL_PROTOCOL_H
#define MSAP1_RPU_CONTROL_PROTOCOL_H

/*
 * Wire protocol shared between the APU application (msap1-apu-app) and the
 * RPU firmware. This header is the single source of truth for the on-wire
 * frame layout. Identifier names are local to each side; the numeric values
 * and packed structure layout form the wire ABI and must remain compatible.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MSAP1_RPU_MAGIC 0x4d525055u
#define MSAP1_RPU_VERSION 2u
#define MSAP1_RPU_MAX_FRAME_SIZE 256u

enum msap1_rpu_msg_type {
	MSAP1_RPU_MSG_PING = 1,
	MSAP1_RPU_MSG_PONG = 2,
	MSAP1_RPU_MSG_GET_STATUS = 3,
	MSAP1_RPU_MSG_STATUS = 4,
	MSAP1_RPU_MSG_SET_LED = 5,
	MSAP1_RPU_MSG_ACK = 6,
	MSAP1_RPU_MSG_ERROR = 7,
	MSAP1_RPU_MSG_ADC_CAPTURE_START = 8,
	MSAP1_RPU_MSG_ADC_CAPTURE_STOP = 9,
	MSAP1_RPU_MSG_RESERVED_10 = 10,
	MSAP1_RPU_MSG_ADC_HEALTH_GET = 11,
	MSAP1_RPU_MSG_ADC_HEALTH = 12,
	MSAP1_RPU_MSG_METER_CONFIG_SET = 13,
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
	MSAP1_RPU_STATUS_ADC_STATE = 8,
	MSAP1_RPU_STATUS_METER_UNAVAILABLE = 9,
	MSAP1_RPU_STATUS_METER_CONFIG = 10,
};

enum msap1_rpu_led_mode {
	MSAP1_RPU_LED_OFF = 0,
	MSAP1_RPU_LED_ON = 1,
	MSAP1_RPU_LED_TOGGLE = 2,
	MSAP1_RPU_LED_HEARTBEAT = 3,
};

enum msap1_adc_health_flag {
	MSAP1_ADC_HEALTH_SPI_RESPONSIVE = 1u << 0,
	MSAP1_ADC_HEALTH_INITIALIZED = 1u << 1,
	MSAP1_ADC_HEALTH_INIT_COMPLETE = 1u << 2,
	MSAP1_ADC_HEALTH_CONFIG_MATCH = 1u << 3,
	MSAP1_ADC_HEALTH_CAPTURE_ACTIVE = 1u << 4,
	MSAP1_ADC_HEALTH_NO_OVERFLOW = 1u << 5,
	MSAP1_ADC_HEALTH_HEADERS_VALID = 1u << 6,
};

enum msap1_adc_spi_health_error {
	MSAP1_ADC_SPI_HEALTH_OK = 0,
	MSAP1_ADC_SPI_HEALTH_NOT_INITIALIZED = 1,
	MSAP1_ADC_SPI_HEALTH_TRANSFER_FAILED = 2,
	MSAP1_ADC_SPI_HEALTH_PROTOCOL_FAILED = 3,
	MSAP1_ADC_SPI_HEALTH_INTERNAL_ERROR = 4,
};

enum msap1_meter_config_flag {
	MSAP1_METER_CONFIG_ENABLE = 1u << 0,
	MSAP1_METER_CONFIG_REMOVE_DC = 1u << 1,
};

enum msap1_frequency_config_flag {
	MSAP1_FREQUENCY_CONFIG_ENABLE = 1u << 0,
};

enum msap1_frequency_mode {
	MSAP1_FREQUENCY_MODE_SINGLE_CYCLE = 0,
	MSAP1_FREQUENCY_MODE_ROLLING_CYCLES = 1,
	MSAP1_FREQUENCY_MODE_ROLLING_TIME = 2,
};

enum msap1_meter_health_flag {
	MSAP1_METER_HEALTH_CORES_PRESENT = 1u << 0,
	MSAP1_METER_HEALTH_CONFIGURED = 1u << 1,
	MSAP1_METER_HEALTH_GENERATION_MATCH = 1u << 2,
	MSAP1_METER_HEALTH_ENABLED = 1u << 3,
	MSAP1_METER_HEALTH_REMOVE_DC = 1u << 4,
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

/*
 * Software-defined PL metering configuration. Coefficients are unsigned
 * Q16.16 micro-units per ADC count. The lower eight bits of valid_mask select
 * configured channels; all remaining bits must be zero. adc_pga_gain carries
 * the human-readable AD7771 gain factor (1, 2, 4, or 8) for every channel.
 */
struct msap1_meter_config_payload {
	uint32_t generation;
	uint32_t sample_rate_hz;
	uint32_t rms_window_samples;
	uint32_t valid_mask;
	uint32_t scale_micro_units_q16[8];
	uint32_t flags;
	uint8_t adc_pga_gain[8];
	/* Frequency fields use millihertz, samples, and integer microvolts. */
	uint32_t frequency_flags;
	uint32_t frequency_mode;
	uint32_t frequency_reference_channel;
	uint32_t frequency_averaging_cycles;
	uint32_t frequency_window_samples;
	uint32_t frequency_minimum_millihz;
	uint32_t frequency_maximum_millihz;
	uint32_t frequency_hysteresis_microvolts;
} __attribute__((packed));

struct msap1_meter_config_ack_payload {
	uint32_t generation;
	uint32_t conversion_active_generation;
	uint32_t processing_active_generation;
	uint32_t conversion_status;
	uint32_t processing_status;
} __attribute__((packed));

/*
 * Active AD7771 SPI readback plus the current PL capture counters. The RPU
 * owns SPI and capture control; Linux owns AXI DMA and obtains ADC health only
 * through this response. START and STOP carry no payload.
 */
struct msap1_adc_health_payload {
	uint32_t health_flags;
	uint32_t sample_rate_hz;
	uint32_t capture_flags;
	uint32_t frame_count;
	uint32_t overflow_count;
	uint32_t header_error_count;
	uint32_t alert_count;
	uint32_t packet_count;
	uint32_t dclk_frequency_hz;
	uint32_t spi_error;
	uint16_t expected_decimation;
	uint8_t status_3;
	uint8_t general_user_config_1;
	uint8_t general_user_config_2;
	uint8_t general_user_config_3;
	uint8_t dout_format;
	uint8_t src_n_msb;
	uint8_t src_n_lsb;
	uint8_t src_if_msb;
	uint8_t src_if_lsb;
	uint8_t src_update;
	uint32_t meter_health_flags;
	uint32_t meter_generation;
	uint32_t conversion_status;
	uint32_t processing_status;
} __attribute__((packed));

#ifdef __cplusplus
}
#endif

#endif /* MSAP1_RPU_CONTROL_PROTOCOL_H */
