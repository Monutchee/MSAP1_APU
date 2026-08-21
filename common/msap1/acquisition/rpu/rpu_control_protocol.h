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
#define MSAP1_RPU_VERSION 5u
/*
 * Stack-buffer bound for one protocol frame on both sides. Must stay
 * under the OpenAMP RPMsg buffer payload (496 bytes on this platform).
 */
#define MSAP1_RPU_MAX_FRAME_SIZE 384u

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
	MSAP1_RPU_MSG_ADC_DIAGNOSTIC_RUN = 14,
	MSAP1_RPU_MSG_ADC_DIAGNOSTIC = 15,
	MSAP1_RPU_MSG_SIMULATOR_EVENT_SET = 16,
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
	MSAP1_ADC_HEALTH_RATE_MATCH = 1u << 7,
	MSAP1_ADC_HEALTH_SIMULATOR_HEALTHY = 1u << 8,
	MSAP1_ADC_HEALTH_PHYSICAL_DIAGNOSTICS = 1u << 9,
};

enum msap1_adc_spi_health_error {
	MSAP1_ADC_SPI_HEALTH_OK = 0,
	MSAP1_ADC_SPI_HEALTH_NOT_INITIALIZED = 1,
	MSAP1_ADC_SPI_HEALTH_TRANSFER_FAILED = 2,
	MSAP1_ADC_SPI_HEALTH_PROTOCOL_FAILED = 3,
	MSAP1_ADC_SPI_HEALTH_INTERNAL_ERROR = 4,
	MSAP1_ADC_SPI_HEALTH_NOT_APPLICABLE = 5,
};

enum msap1_adc_source {
	MSAP1_ADC_SOURCE_PHYSICAL = 0,
	MSAP1_ADC_SOURCE_SIMULATOR = 1,
};

enum msap1_adc_diagnostic_flag {
	MSAP1_ADC_DIAGNOSTIC_RESET_ASSERTED = 1u << 0,
	MSAP1_ADC_DIAGNOSTIC_RESET_DRDY_STOPPED = 1u << 1,
	MSAP1_ADC_DIAGNOSTIC_RESET_DEFAULTS_READ = 1u << 2,
	MSAP1_ADC_DIAGNOSTIC_SRC_UPDATE_HIGH_READ = 1u << 3,
	MSAP1_ADC_DIAGNOSTIC_SRC_UPDATE_LOW_READ = 1u << 4,
	MSAP1_ADC_DIAGNOSTIC_SRC_HOLDING_MATCH = 1u << 5,
	MSAP1_ADC_DIAGNOSTIC_FINAL_CONFIG_MATCH = 1u << 6,
	MSAP1_ADC_DIAGNOSTIC_FINAL_DRDY_MATCH = 1u << 7,
};

enum msap1_adc_diagnostic_snapshot_flag {
	MSAP1_ADC_DIAGNOSTIC_SNAPSHOT_SPI_VALID = 1u << 0,
};

enum msap1_adc_diagnostic_stage {
	MSAP1_ADC_DIAGNOSTIC_STAGE_NONE = 0,
	MSAP1_ADC_DIAGNOSTIC_STAGE_PREFLIGHT = 1,
	MSAP1_ADC_DIAGNOSTIC_STAGE_BEFORE = 2,
	MSAP1_ADC_DIAGNOSTIC_STAGE_RESET_ASSERT = 3,
	MSAP1_ADC_DIAGNOSTIC_STAGE_RESET_RELEASE = 4,
	MSAP1_ADC_DIAGNOSTIC_STAGE_RESET_DEFAULTS = 5,
	MSAP1_ADC_DIAGNOSTIC_STAGE_RECONFIGURE = 6,
	MSAP1_ADC_DIAGNOSTIC_STAGE_AFTER = 7,
};

enum msap1_adc_diagnostic_error {
	MSAP1_ADC_DIAGNOSTIC_ERROR_NONE = 0,
	MSAP1_ADC_DIAGNOSTIC_ERROR_NOT_INITIALIZED = 1,
	MSAP1_ADC_DIAGNOSTIC_ERROR_CAPTURE_ACTIVE = 2,
	MSAP1_ADC_DIAGNOSTIC_ERROR_SPI = 3,
	MSAP1_ADC_DIAGNOSTIC_ERROR_ADC_NOT_READY = 4,
	MSAP1_ADC_DIAGNOSTIC_ERROR_REGISTER_MISMATCH = 5,
	MSAP1_ADC_DIAGNOSTIC_ERROR_INTERNAL = 6,
};

enum msap1_meter_config_flag {
	MSAP1_METER_CONFIG_ENABLE = 1u << 0,
	MSAP1_METER_CONFIG_REMOVE_DC = 1u << 1,
};

enum msap1_frequency_config_flag {
	MSAP1_FREQUENCY_CONFIG_ENABLE = 1u << 0,
};

enum msap1_simulator_config_flag {
	/*
	 * Keep the simulator's phase accumulator, fractional scheduler, and
	 * packet framing across the configuration APPLY, so the generated
	 * waveform continues seamlessly instead of restarting at 0 degrees
	 * (no phase discontinuity into the metrology engines).
	 */
	MSAP1_SIMULATOR_FLAG_PRESERVE_PHASE = 1u << 0,
};

/*
 * Simulator event sequencer actions (metrology M12). Deliberately a
 * message of its own rather than fields in the configuration payload: a
 * configuration commit stops and restarts capture, which would destroy
 * the phase continuity the event exists to preserve. An event is armed
 * against a running, unchanged configuration.
 */
enum msap1_simulator_event_action {
	/* Commit the burst description below and start it at the
	 * generator's next half-cycle boundary. */
	MSAP1_SIMULATOR_EVENT_ARM = 0,
	/* Drop the envelope immediately; the burst is not counted. */
	MSAP1_SIMULATOR_EVENT_CANCEL = 1,
	/* Zero the completed-burst counter (bookkeeping between
	 * scenarios); leaves any running burst alone. */
	MSAP1_SIMULATOR_EVENT_CLEAR_COUNT = 2,
	/* Read the sequencer state without changing anything. */
	MSAP1_SIMULATOR_EVENT_QUERY = 3,
};

enum msap1_simulator_event_flag {
	/* Re-fire the burst every period_half_cycles until cancelled. */
	MSAP1_SIMULATOR_EVENT_FLAG_REPEAT = 1u << 0,
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
	/* Raw-sample source and simulator values. Phases are Q0.32 turns. */
	uint32_t adc_source;
	uint32_t simulator_frequency_millihz;
	uint32_t simulator_valid_mask;
	int32_t simulator_peak_counts[8];
	uint32_t simulator_phase_q32[8];
	uint32_t simulator_phase_step_q32;
	/* Signed DC offset per channel, ADC counts. */
	int32_t simulator_dc_offset_counts[8];
	/* Uniform fluctuation amplitude per channel, ADC counts: the PL adds
	 * white noise in +/- level (RMS contribution level/sqrt(3)); 0 keeps
	 * the channel noise-free. */
	uint32_t simulator_noise_level_counts[8];
	/* Four harmonic slots, two packed words each (word0 = order[7:0] |
	 * channel mask[15:8] | Q16 fraction-of-fundamental[31:16]; word1 =
	 * phase, Q0.32 turns). All-zero slots are disabled. */
	uint32_t simulator_harmonics[8];
	/* MSAP1_SIMULATOR_FLAG_* bits. */
	uint32_t simulator_flags;
	/*
	 * Declared nominal grid frequency in hertz. Only 50 or 60 is valid. This
	 * is configuration, not the measured frequency; it selects the Class A
	 * basic measurement block length (50 Hz -> 10 cycles, 60 Hz -> 12 cycles)
	 * that the RPU programs into the PL grid-cycle timing registers.
	 */
	uint32_t nominal_frequency_hz;
	/*
	 * IEC 61000-4-30 Urms(1/2) event detection (metrology M12).
	 * pq_reference_microvolts is the declared reference Udin; ZERO
	 * DISABLES DETECTION -- the PL keeps publishing Urms(1/2)
	 * snapshots but never declares an event, so an unconfigured
	 * reference cannot invent dips. The four thresholds are fractions
	 * of that reference in units of 1e-4 (9000 = 90.00 %).
	 */
	uint32_t pq_reference_microvolts;
	uint32_t pq_sag_threshold_e4;
	uint32_t pq_swell_threshold_e4;
	uint32_t pq_interruption_threshold_e4;
	uint32_t pq_hysteresis_e4;
} __attribute__((packed));

/*
 * One event-sequencer command. The burst description is ignored for
 * every action but ARM.
 */
struct msap1_simulator_event_payload {
	uint32_t action;                /* msap1_simulator_event_action */
	uint32_t channel_mask;          /* [7:0] lanes the envelope scales */
	/* Unsigned Q16 amplitude multiplier: 0x10000 unity, 0 a full
	 * interruption, 0xE666 a 10 % sag. Clamped at 4.0 by the PL. */
	uint32_t scale_q16;
	/* Burst length in HALF CYCLES of the generated waveform; 1..65535
	 * (zero is rejected). Half-cycle units because Urms(1/2) refreshes
	 * every half cycle. */
	uint32_t duration_half_cycles;
	/* Repeat period in half cycles, measured start to start; at or
	 * below the duration the bursts run back to back. */
	uint32_t period_half_cycles;
	uint32_t flags;                 /* MSAP1_SIMULATOR_EVENT_FLAG_* */
} __attribute__((packed));

/* The sequencer state after the command, read straight from the PL. */
struct msap1_simulator_event_ack_payload {
	/* [0] armed, [1] running, [2] holding, [31:16] bursts completed. */
	uint32_t status;
	/* Half cycles left in the burst [15:0] and until the next repeat
	 * [31:16]. */
	uint32_t remaining;
	uint32_t active_control;
	uint32_t active_scale;
	uint32_t active_timing;
} __attribute__((packed));

struct msap1_meter_config_ack_payload {
	uint32_t generation;
	uint32_t conversion_active_generation;
	uint32_t processing_active_generation;
	uint32_t conversion_status;
	uint32_t processing_status;
	uint32_t adc_source;
	uint32_t simulator_active_generation;
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
	uint32_t drdy_frequency_hz;
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
	uint8_t channel_config[8];       /* 0x00 through 0x07 */
	uint8_t channel_error[8];        /* 0x4C through 0x53 */
	uint8_t saturation_error[4];     /* 0x54 through 0x57 */
	uint8_t channel_error_enable;    /* 0x58 */
	uint8_t general_error_1;         /* 0x59 */
	uint8_t general_error_1_enable;  /* 0x5A */
	uint8_t general_error_2;         /* 0x5B */
	uint8_t general_error_2_enable;  /* 0x5C */
	uint8_t status_1;                /* 0x5D */
	uint8_t status_2;                /* 0x5E */
	uint8_t channel_disable;         /* 0x08 */
	uint8_t channel_sync_offset[8];  /* 0x09 through 0x10 */
	uint8_t adc_mux_config;          /* 0x15 */
	uint8_t global_mux_config;       /* 0x16 */
	uint8_t gpio_config;             /* 0x17 */
	uint8_t gpio_data;               /* 0x18 */
	uint8_t buffer_config_1;         /* 0x19 */
	uint8_t buffer_config_2;         /* 0x1A */
	uint8_t channel_offset[8][3];    /* 0x1C through 0x48, stride 6 */
	uint8_t channel_gain[8][3];      /* 0x1F through 0x4B, stride 6 */
	uint32_t spi_protocol_error_count; /* cumulative malformed headers */
	uint32_t spi_retry_recovery_count; /* reads recovered by retry */
	/* Configuration reads whose two samples disagreed. Counts the
	 * data-byte corruption the header check structurally cannot see. */
	uint32_t spi_config_read_mismatch_count;
	/* Health polls that found GEN_ERR_REG_1 (0x59) non-zero. That
	 * register is clear-on-read, so reading it during the sweep is also
	 * what clears it -- these two fields are the only lasting record. */
	uint32_t spi_general_error_1_events;
	uint8_t spi_last_failed_register;
	uint8_t spi_last_received_header;
	/* Sticky OR of every non-zero GEN_ERR_REG_1 sample. Bit 1
	 * SPI_CRC_ERR, 2 SPI_INVALID_WRITE_ERR, 3 SPI_INVALID_READ_ERR,
	 * 4 SPI_CLK_COUNT_ERR, 5 ROM_CRC_ERR, 6 MEMMAP_CRC_ERR. */
	uint8_t spi_general_error_1_sticky;
	uint8_t spi_diagnostics_reserved[1];
	/* Malformed reply headers bucketed by high nibble, saturating at
	 * 0xFFFF. The only valid header is 0x20, so a healthy bus leaves
	 * every bucket zero. A single "last header" byte cannot tell a
	 * systematic corruption from random mis-sampling; the shape here
	 * can, and that distinction selects the next fix. */
	uint16_t spi_header_histogram[16];
	uint32_t adc_source;
	uint32_t simulator_status;
	uint32_t simulator_active_generation;
	uint32_t simulator_frame_count;
	uint32_t simulator_saturation_count;
	uint32_t simulator_missed_sample_count;
} __attribute__((packed));

struct msap1_adc_diagnostic_request {
	uint32_t flow;
} __attribute__((packed));

/*
 * A compact capture/register snapshot used by the destructive ADC diagnostic.
 * Register bytes are valid only when SNAPSHOT_SPI_VALID is set. The reset
 * snapshot deliberately has no SPI data because RESET_N is asserted.
 */
struct msap1_adc_diagnostic_snapshot {
	uint32_t snapshot_flags;
	uint32_t capture_flags;
	uint32_t frame_count;
	uint32_t packet_count;
	uint32_t dclk_frequency_hz;
	uint32_t drdy_frequency_hz;
	uint8_t status_1;
	uint8_t status_2;
	uint8_t status_3;
	uint8_t general_user_config_1;
	uint8_t general_user_config_2;
	uint8_t general_user_config_3;
	uint8_t dout_format;
	uint8_t channel_disable;
	uint8_t buffer_config_1;
	uint8_t buffer_config_2;
	uint8_t src_n_msb;
	uint8_t src_n_lsb;
	uint8_t src_if_msb;
	uint8_t src_if_lsb;
	uint8_t src_update;
	uint8_t reserved;
} __attribute__((packed));

/*
 * Flow 1 is a warm hardware-pin diagnostic, not a board power cycle:
 *   before -> assert ADC RESET_N -> reset defaults -> conservative SRC load
 *   -> after.
 * The PL/FPGA and Linux remain running while only the sensor-board ADC reset
 * output is pulsed.
 */
struct msap1_adc_diagnostic_payload {
	uint32_t flow;
	uint32_t requested_sample_rate_hz;
	uint32_t diagnostic_flags;
	uint32_t diagnostic_error;
	uint32_t failure_stage;
	uint32_t reset_hold_ms;
	uint8_t src_update_high_readback;
	uint8_t src_update_low_readback;
	uint8_t reserved[2];
	struct msap1_adc_diagnostic_snapshot before;
	struct msap1_adc_diagnostic_snapshot reset_asserted;
	struct msap1_adc_diagnostic_snapshot reset_defaults;
	struct msap1_adc_diagnostic_snapshot after;
} __attribute__((packed));

#ifdef __cplusplus
}
#endif

#endif /* MSAP1_RPU_CONTROL_PROTOCOL_H */
