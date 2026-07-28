#include "cli.hpp"
#include "result_output.hpp"

#include "msap1/acquisition_ipc.hpp"
#include "msap1/meter_health.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace msap1::cli {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

volatile std::sig_atomic_t stop_requested = 0;

std::uint64_t parse_positive_integer(const std::string &value,
				     const std::string &option)
{
	std::size_t end = 0;
	std::uint64_t result = 0;
	try {
		result = std::stoull(value, &end, 0);
	} catch (const std::exception &) {
		throw std::invalid_argument(option + " requires a positive integer");
	}
	if (end != value.size() || result == 0)
		throw std::invalid_argument(option + " requires a positive integer");
	return result;
}

double parse_duration(const std::string &value)
{
	std::size_t end = 0;
	double result = 0.0;
	try {
		result = std::stod(value, &end);
	} catch (const std::exception &) {
		throw std::invalid_argument("--duration requires a positive number");
	}
	if (end != value.size() || !std::isfinite(result) || result <= 0.0)
		throw std::invalid_argument("--duration requires a positive number");
	return result;
}

const char *yes_no(bool value) { return value ? "yes" : "no"; }

void print_adc_register(std::ostream &output, std::uint8_t address,
			const char *name, std::uint8_t value)
{
	const auto flags = output.flags();
	const auto fill = output.fill();
	output << "    0x" << std::hex << std::uppercase << std::setw(2)
	       << std::setfill('0') << static_cast<unsigned>(address) << "  "
	       << std::left << std::setw(25) << std::setfill(' ') << name
	       << " = 0x" << std::right << std::setw(2) << std::setfill('0')
	       << static_cast<unsigned>(value) << '\n';
	output.flags(flags);
	output.fill(fill);
}

void print_adc_registers(std::ostream &output,
			 const msap1_adc_health_payload &health)
{
	static constexpr std::array<const char *, 8> channel_config_names{
		"CH0_CONFIG", "CH1_CONFIG", "CH2_CONFIG", "CH3_CONFIG",
		"CH4_CONFIG", "CH5_CONFIG", "CH6_CONFIG", "CH7_CONFIG"};
	static constexpr std::array<const char *, 8> channel_error_names{
		"CH0_ERR_REG", "CH1_ERR_REG", "CH2_ERR_REG", "CH3_ERR_REG",
		"CH4_ERR_REG", "CH5_ERR_REG", "CH6_ERR_REG", "CH7_ERR_REG"};
	static constexpr std::array<const char *, 8> channel_sync_names{
		"CH0_SYNC_OFFSET", "CH1_SYNC_OFFSET", "CH2_SYNC_OFFSET",
		"CH3_SYNC_OFFSET", "CH4_SYNC_OFFSET", "CH5_SYNC_OFFSET",
		"CH6_SYNC_OFFSET", "CH7_SYNC_OFFSET"};
	static constexpr std::array<const char *, 4> saturation_error_names{
		"CH0_1_SAT_ERR", "CH2_3_SAT_ERR",
		"CH4_5_SAT_ERR", "CH6_7_SAT_ERR"};

	output << "\n  AD7771 register snapshot:\n";
	for (std::size_t channel = 0; channel < channel_config_names.size();
	     ++channel)
		print_adc_register(output, static_cast<std::uint8_t>(channel),
				   channel_config_names[channel],
				   health.channel_config[channel]);
	print_adc_register(output, 0x08, "CH_DISABLE", health.channel_disable);
	for (std::size_t channel = 0; channel < channel_sync_names.size();
	     ++channel)
		print_adc_register(output,
				   static_cast<std::uint8_t>(0x09 + channel),
				   channel_sync_names[channel],
				   health.channel_sync_offset[channel]);
	print_adc_register(output, 0x11, "GENERAL_USER_CONFIG_1",
			   health.general_user_config_1);
	print_adc_register(output, 0x12, "GENERAL_USER_CONFIG_2",
			   health.general_user_config_2);
	print_adc_register(output, 0x13, "GENERAL_USER_CONFIG_3",
			   health.general_user_config_3);
	print_adc_register(output, 0x14, "DOUT_FORMAT", health.dout_format);
	print_adc_register(output, 0x15, "ADC_MUX_CONFIG",
			   health.adc_mux_config);
	print_adc_register(output, 0x16, "GLOBAL_MUX_CONFIG",
			   health.global_mux_config);
	print_adc_register(output, 0x17, "GPIO_CONFIG", health.gpio_config);
	print_adc_register(output, 0x18, "GPIO_DATA", health.gpio_data);
	print_adc_register(output, 0x19, "BUFFER_CONFIG_1",
			   health.buffer_config_1);
	print_adc_register(output, 0x1a, "BUFFER_CONFIG_2",
			   health.buffer_config_2);
	for (std::size_t channel = 0; channel < channel_config_names.size();
	     ++channel) {
		const auto base = static_cast<std::uint8_t>(0x1c + channel * 6);
		const auto offset_name = std::string("CH") +
			std::to_string(channel) + "_OFFSET";
		const auto gain_name = std::string("CH") +
			std::to_string(channel) + "_GAIN";
		for (std::size_t byte = 0; byte < 3; ++byte) {
			const auto offset_byte_name = offset_name +
				(byte == 0 ? "_UPPER" :
				 byte == 1 ? "_MIDDLE" : "_LOWER");
			const auto gain_byte_name = gain_name +
				(byte == 0 ? "_UPPER" :
				 byte == 1 ? "_MIDDLE" : "_LOWER");
			print_adc_register(output,
				static_cast<std::uint8_t>(base + byte),
				offset_byte_name.c_str(),
				health.channel_offset[channel][byte]);
			print_adc_register(output,
				static_cast<std::uint8_t>(base + 3 + byte),
				gain_byte_name.c_str(),
				health.channel_gain[channel][byte]);
		}
	}
	for (std::size_t channel = 0; channel < channel_error_names.size();
	     ++channel)
		print_adc_register(output,
				   static_cast<std::uint8_t>(0x4c + channel),
				   channel_error_names[channel],
				   health.channel_error[channel]);
	for (std::size_t pair = 0; pair < saturation_error_names.size(); ++pair)
		print_adc_register(output, static_cast<std::uint8_t>(0x54 + pair),
				   saturation_error_names[pair],
				   health.saturation_error[pair]);
	print_adc_register(output, 0x58, "CHX_ERR_REG_EN",
			   health.channel_error_enable);
	print_adc_register(output, 0x59, "GEN_ERR_REG_1",
			   health.general_error_1);
	print_adc_register(output, 0x5a, "GEN_ERR_REG_1_EN",
			   health.general_error_1_enable);
	print_adc_register(output, 0x5b, "GEN_ERR_REG_2",
			   health.general_error_2);
	print_adc_register(output, 0x5c, "GEN_ERR_REG_2_EN",
			   health.general_error_2_enable);
	print_adc_register(output, 0x5d, "STATUS_REG_1", health.status_1);
	print_adc_register(output, 0x5e, "STATUS_REG_2", health.status_2);
	print_adc_register(output, 0x5f, "STATUS_REG_3", health.status_3);
	print_adc_register(output, 0x60, "SRC_N_MSB", health.src_n_msb);
	print_adc_register(output, 0x61, "SRC_N_LSB", health.src_n_lsb);
	print_adc_register(output, 0x62, "SRC_IF_MSB", health.src_if_msb);
	print_adc_register(output, 0x63, "SRC_IF_LSB", health.src_if_lsb);
	print_adc_register(output, 0x64, "SRC_UPDATE", health.src_update);

	const auto high_resolution =
		(health.general_user_config_1 & 0x40u) != 0u;
	const auto sinc5 = (health.general_user_config_2 & 0x40u) != 0u;
	const auto dout_code = (health.dout_format >> 6) & 0x03u;
	const auto dout_lines = dout_code == 0u ? 4u :
		dout_code == 1u ? 2u : 1u;
	const auto dclk_divisor =
		1u << ((health.dout_format >> 1) & 0x07u);
	const auto src_n = static_cast<std::uint16_t>(
		(static_cast<std::uint16_t>(health.src_n_msb & 0x0fu) << 8) |
		health.src_n_lsb);
	const auto src_if = static_cast<std::uint16_t>(
		(static_cast<std::uint16_t>(health.src_if_msb) << 8) |
		health.src_if_lsb);
	const auto effective_decimation =
		static_cast<double>(src_n) +
		static_cast<double>(src_if) / 65536.0;
	const auto reference_mux = (health.adc_mux_config >> 6) & 0x03u;
	static constexpr std::array<const char *, 4> reference_names{
		"external REFx+/REFx-", "internal reference",
		"AVDD1x/AVSSx", "reversed external REFx-/REFx+"};

	output << "\n  AD7771 decoded controls:\n"
	       << "    Power mode                = "
	       << (high_resolution ? "high resolution" : "low power")
	       << "\n    Digital filter            = "
	       << (sinc5 ? "sinc5" : "sinc3")
	       << "\n    Enabled channels          = 0x" << std::hex
	       << std::uppercase << std::setw(2) << std::setfill('0')
	       << static_cast<unsigned>(~health.channel_disable & 0xffu)
	       << std::dec << std::nouppercase << std::setfill(' ')
	       << "\n    DOUT lanes                = " << dout_lines
	       << "\n    DOUT header               = "
	       << ((health.dout_format & 0x20u) != 0u ? "CRC" : "status")
	       << "\n    DCLK divisor              = " << dclk_divisor
	       << "\n    Reference mux             = "
	       << reference_names[reference_mux]
	       << "\n    SRC decimation            = " << src_n << " + " << src_if
	       << "/65536"
	       << "\n    Expected decimation       = " << health.expected_decimation
	       << "\n    SRC load source           = "
	       << ((health.src_update & 0x80u) != 0u ? "GPIO0" : "software")
	       << "\n    SRC update pending        = "
	       << yes_no((health.src_update & 0x01u) != 0u)
	       << "\n    CHIP_ERROR active         = "
	       << yes_no(((health.status_1 | health.status_2 |
			   health.status_3) & 0x20u) != 0u)
	       << '\n';
	if (health.dclk_frequency_hz != 0u && effective_decimation > 0.0) {
		const auto master_clock_hz =
			static_cast<double>(health.dclk_frequency_hz) *
			static_cast<double>(dclk_divisor);
		const auto modulator_divisor =
			high_resolution ? 4.0 : 8.0;
		output << "    SRC-derived ODR           = " << std::fixed
		       << std::setprecision(3)
		       << master_clock_hz /
			  (modulator_divisor * effective_decimation)
		       << " frame/s\n";
	}
}

void require_daemon_ok(const AcquisitionResponse &response)
{
	if (response.status != AcquisitionStatus::ok)
		throw std::runtime_error("acquisition daemon request failed (status " +
			std::to_string(static_cast<std::uint32_t>(response.status)) + ")");
}

struct MeterHealthResult {
	AcquisitionResponse response;
	MeterHealth status;
	bool full = false;
};

int write_meter_health_text(const MeterHealthResult &result,
			    std::ostream &output)
{
	const auto &response = result.response;
	const auto &status = result.status;
	const auto &health = response.rpu_health;

	if (!result.full) {
		output << "MSAP1 meter health: "
		       << (status.healthy ? "PASS" : "FAIL") << '\n'
		       << "  Acquisition:         "
		       << (status.acquisition_healthy ? "healthy" : "degraded")
		       << " (" << (response.running != 0u ? "running" : "stopped")
		       << ", record ";
		if (response.meter_record_age_ms !=
		    std::numeric_limits<std::uint32_t>::max())
			output << response.meter_record_age_ms << " ms old)\n";
		else
			output << "unavailable)\n";
		output << "  ADC:                 "
		       << (status.adc_healthy ? "healthy" : "degraded") << '\n'
		       << "  Sample rate:         " << health.sample_rate_hz
		       << " configured / ";
		if (health.drdy_frequency_hz != 0u)
			output << health.drdy_frequency_hz;
		else
			output << "unavailable";
		output << " measured frame/s\n"
		       << "  Data-path errors:    DMA " << response.dma_read_errors
		       << ", invalid " << response.invalid_records << ", gaps "
		       << response.sequence_gaps << ", FIFO "
		       << health.overflow_count << ", headers "
		       << health.header_error_count << '\n'
		       << "  Health audit:        ";
		if (response.health_probe_pending != 0u) {
			if (response.health_probe_failures == 0u)
				output << "startup stabilization\n";
			else
				output << "confirmation pending ("
				       << response.health_probe_failures
				       << " failure)\n";
		} else if (response.rpu_health_age_ms !=
			   std::numeric_limits<std::uint32_t>::max()) {
			output << "cached " << response.rpu_health_age_ms
			       << " ms ago\n";
		} else {
			output << "unavailable\n";
		}
		if (!status.adc_degraded_reasons.empty()) {
			output << "  ADC degraded because:\n";
			for (const auto &reason : status.adc_degraded_reasons)
				output << "    - [" << reason.code << "] "
				       << reason.message << '\n';
		}
		output << "  Run 'mnc meter health --full' for complete diagnostics.\n";
		return status.healthy ? 0 : 1;
	}

	output << "MSAP1 meter health: " << (status.healthy ? "PASS" : "FAIL") << '\n'
	       << "  Linux acquisition:    " << yes_no(response.running != 0u) << '\n'
	       << "  Meter record present: " << yes_no(response.has_meter_record != 0u)
	       << '\n'
	       << "  Meter record stale:   " << yes_no(status.record_stale) << '\n'
	       << "  Meter record age:     ";
	if (response.meter_record_age_ms !=
	    std::numeric_limits<std::uint32_t>::max())
		output << response.meter_record_age_ms << " ms\n";
	else
		output << "unavailable\n";
	output << "  RPU health cache age: ";
	if (response.rpu_health_age_ms !=
	    std::numeric_limits<std::uint32_t>::max())
		output << response.rpu_health_age_ms << " ms\n";
	else
		output << "unavailable\n";
	output << "  Health confirmation:  "
	       << (response.health_probe_pending == 0u
			   ? "not pending"
			   : response.health_probe_failures == 0u
				   ? "startup stabilization"
				   : "pending (" +
					     std::to_string(
						     response.health_probe_failures) +
					     " failure)")
	       << '\n'
	       << "  Meter records:        " << response.meter_records << '\n'
	       << "  DMA bytes:            " << response.dma_bytes << '\n'
	       << "  DMA read errors:      " << response.dma_read_errors << '\n'
	       << "  Invalid records:      " << response.invalid_records << '\n'
	       << "  Sequence gaps:        " << response.sequence_gaps << '\n'
	       << "  Configuration gen:    0x" << std::hex
	       << response.configuration_generation << std::dec << '\n'
	       << "  PL generation match:  "
	       << yes_no(status.meter_generation_match) << '\n'
	       << "  Meter configured:     " << yes_no(status.meter_configured) << '\n'
	       << "  DC offset removal:    " << yes_no(status.dc_offset_removal) << '\n'
	       << "  ADC SPI responsive:   " << yes_no(status.spi_responsive) << '\n'
	       << "  ADC rate match:       " << yes_no(status.rate_match) << '\n'
	       << "  Capture active:       " << yes_no(status.capture_active) << '\n'
	       << "  Sample rate:          " << health.sample_rate_hz << " frame/s\n"
	       << "  PL frames:            " << health.frame_count << '\n'
	       << "  ADC packets:          " << health.packet_count << '\n'
	       << "  ADC DCLK:             ";
	if (health.dclk_frequency_hz != 0u)
		output << health.dclk_frequency_hz << " Hz\n";
	else
		output << "unavailable\n";
	output << "  ADC DRDY:             ";
	if (health.drdy_frequency_hz != 0u)
		output << health.drdy_frequency_hz << " frame/s\n";
	else
		output << "unavailable\n";
	output << "  FIFO overflows:       " << health.overflow_count << '\n'
	       << "  Header errors:        " << health.header_error_count << '\n'
	       << "  SPI protocol errors:  "
	       << health.spi_protocol_error_count << '\n'
	       << "  SPI retry recoveries: "
	       << health.spi_retry_recovery_count << '\n'
	       << "  Last SPI failure:     register 0x" << std::hex
	       << static_cast<unsigned int>(health.spi_last_failed_register)
	       << ", header 0x"
	       << static_cast<unsigned int>(health.spi_last_received_header)
	       << std::dec << '\n'
	       << "  Conversion status:   0x" << std::hex << health.conversion_status
	       << '\n'
	       << "  Processing status:   0x" << health.processing_status << std::dec
	       << '\n'
	       << "  Frequency arithmetic: "
	       << (status.frequency_arithmetic_ok ? "ok" : "fault") << '\n';
	if (!status.adc_degraded_reasons.empty()) {
		output << "  ADC degraded because:\n";
		for (const auto &reason : status.adc_degraded_reasons)
			output << "    - [" << reason.code << "] "
			       << reason.message << '\n';
	}
	if (status.spi_responsive)
		print_adc_registers(output, health);
	else
		output << "\n  AD7771 register snapshot: unavailable\n";
	return status.healthy ? 0 : 1;
}

struct HealthReasonDto {
	std::string code;
	std::string message;
};

struct RegisterDto {
	std::uint32_t address = 0;
	std::string name;
	std::uint32_t value = 0;
};

struct AcquisitionHealthDto {
	bool healthy = false;
	bool running = false;
	bool record_present = false;
	bool record_stale = false;
	std::optional<std::uint32_t> record_age_ms;
	std::uint64_t records = 0;
	std::uint64_t dma_bytes = 0;
	std::uint64_t dma_read_errors = 0;
	std::uint64_t invalid_records = 0;
	std::uint64_t sequence_gaps = 0;
};

struct AdcHealthDto {
	bool healthy = false;
	bool spi_responsive = false;
	bool initialized = false;
	bool configuration_match = false;
	bool rate_match = false;
	bool capture_active = false;
	bool fifo_ok = false;
	bool headers_valid = false;
	std::uint32_t configured_rate_hz = 0;
	std::uint32_t measured_drdy_hz = 0;
	std::uint32_t measured_dclk_hz = 0;
	std::uint64_t frames = 0;
	std::uint64_t packets = 0;
	std::uint32_t fifo_overflows = 0;
	std::uint32_t header_errors = 0;
	std::uint32_t spi_error = 0;
	std::vector<HealthReasonDto> degraded_reasons;
	std::vector<RegisterDto> registers;
};

struct MeterHealthDto {
	bool healthy = false;
	bool full = false;
	std::uint32_t configuration_generation = 0;
	bool meter_configured = false;
	bool meter_generation_match = false;
	bool dc_offset_removal = false;
	bool frequency_arithmetic_ok = false;
	std::optional<std::uint32_t> rpu_health_age_ms;
	bool health_probe_pending = false;
	std::uint32_t health_probe_failures = 0;
	AcquisitionHealthDto acquisition;
	AdcHealthDto adc;
};

std::vector<RegisterDto>
adc_register_dtos(const msap1_adc_health_payload &health)
{
	std::vector<RegisterDto> registers;
	auto add = [&registers](std::uint32_t address, std::string name,
			       std::uint8_t value) {
		registers.push_back({address, std::move(name), value});
	};
	for (std::size_t channel = 0; channel < 8; ++channel)
		add(channel, "CH" + std::to_string(channel) + "_CONFIG",
		    health.channel_config[channel]);
	add(0x08, "CH_DISABLE", health.channel_disable);
	for (std::size_t channel = 0; channel < 8; ++channel)
		add(0x09 + channel,
		    "CH" + std::to_string(channel) + "_SYNC_OFFSET",
		    health.channel_sync_offset[channel]);
	add(0x11, "GENERAL_USER_CONFIG_1", health.general_user_config_1);
	add(0x12, "GENERAL_USER_CONFIG_2", health.general_user_config_2);
	add(0x13, "GENERAL_USER_CONFIG_3", health.general_user_config_3);
	add(0x14, "DOUT_FORMAT", health.dout_format);
	add(0x15, "ADC_MUX_CONFIG", health.adc_mux_config);
	add(0x16, "GLOBAL_MUX_CONFIG", health.global_mux_config);
	add(0x17, "GPIO_CONFIG", health.gpio_config);
	add(0x18, "GPIO_DATA", health.gpio_data);
	add(0x19, "BUFFER_CONFIG_1", health.buffer_config_1);
	add(0x1a, "BUFFER_CONFIG_2", health.buffer_config_2);
	for (std::size_t channel = 0; channel < 8; ++channel) {
		const auto base = 0x1cu + channel * 6u;
		for (std::size_t byte = 0; byte < 3; ++byte) {
			add(base + byte,
			    "CH" + std::to_string(channel) + "_OFFSET_" +
				    std::to_string(byte),
			    health.channel_offset[channel][byte]);
			add(base + 3u + byte,
			    "CH" + std::to_string(channel) + "_GAIN_" +
				    std::to_string(byte),
			    health.channel_gain[channel][byte]);
		}
	}
	for (std::size_t channel = 0; channel < 8; ++channel)
		add(0x4cu + channel,
		    "CH" + std::to_string(channel) + "_ERR_REG",
		    health.channel_error[channel]);
	for (std::size_t pair = 0; pair < 4; ++pair)
		add(0x54u + pair, "SATURATION_ERR_" + std::to_string(pair),
		    health.saturation_error[pair]);
	add(0x58, "CHX_ERR_REG_EN", health.channel_error_enable);
	add(0x59, "GEN_ERR_REG_1", health.general_error_1);
	add(0x5a, "GEN_ERR_REG_1_EN", health.general_error_1_enable);
	add(0x5b, "GEN_ERR_REG_2", health.general_error_2);
	add(0x5c, "GEN_ERR_REG_2_EN", health.general_error_2_enable);
	add(0x5d, "STATUS_REG_1", health.status_1);
	add(0x5e, "STATUS_REG_2", health.status_2);
	add(0x5f, "STATUS_REG_3", health.status_3);
	add(0x60, "SRC_N_MSB", health.src_n_msb);
	add(0x61, "SRC_N_LSB", health.src_n_lsb);
	add(0x62, "SRC_IF_MSB", health.src_if_msb);
	add(0x63, "SRC_IF_LSB", health.src_if_lsb);
	add(0x64, "SRC_UPDATE", health.src_update);
	return registers;
}

MeterHealthDto meter_health_dto(const MeterHealthResult &result)
{
	const auto &response = result.response;
	const auto &status = result.status;
	const auto &health = response.rpu_health;
	MeterHealthDto dto{
		.healthy = status.healthy,
		.full = result.full,
		.configuration_generation = response.configuration_generation,
		.meter_configured = status.meter_configured,
		.meter_generation_match = status.meter_generation_match,
		.dc_offset_removal = status.dc_offset_removal,
		.frequency_arithmetic_ok = status.frequency_arithmetic_ok,
		.rpu_health_age_ms =
			response.rpu_health_age_ms ==
					std::numeric_limits<std::uint32_t>::max()
				? std::nullopt
				: std::optional<std::uint32_t>(
					  response.rpu_health_age_ms),
		.health_probe_pending = response.health_probe_pending != 0u,
		.health_probe_failures = response.health_probe_failures,
		.acquisition = {
			.healthy = status.acquisition_healthy,
			.running = response.running != 0u,
			.record_present = response.has_meter_record != 0u,
			.record_stale = status.record_stale,
			.record_age_ms =
				response.meter_record_age_ms ==
						std::numeric_limits<std::uint32_t>::max()
					? std::nullopt
					: std::optional<std::uint32_t>(
						  response.meter_record_age_ms),
			.records = response.meter_records,
			.dma_bytes = response.dma_bytes,
			.dma_read_errors = response.dma_read_errors,
			.invalid_records = response.invalid_records,
			.sequence_gaps = response.sequence_gaps,
		},
		.adc = {
			.healthy = status.adc_healthy,
			.spi_responsive = status.spi_responsive,
			.initialized = status.initialized,
			.configuration_match = status.configuration_match,
			.rate_match = status.rate_match,
			.capture_active = status.capture_active,
			.fifo_ok = status.fifo_ok,
			.headers_valid = status.headers_valid,
			.configured_rate_hz = health.sample_rate_hz,
			.measured_drdy_hz = health.drdy_frequency_hz,
			.measured_dclk_hz = health.dclk_frequency_hz,
			.frames = health.frame_count,
			.packets = health.packet_count,
			.fifo_overflows = health.overflow_count,
			.header_errors = health.header_error_count,
			.spi_error = health.spi_error,
			.degraded_reasons = {},
			.registers = {},
		},
	};
	for (const auto &reason : status.adc_degraded_reasons)
		dto.adc.degraded_reasons.push_back({reason.code, reason.message});
	if (result.full && status.spi_responsive)
		dto.adc.registers = adc_register_dtos(health);
	return dto;
}

class MeterHealthTextGenerator final :
	public ResultGenerator<MeterHealthResult> {
public:
	int write(const MeterHealthResult &result,
		  std::ostream &output) const override
	{
		return write_meter_health_text(result, output);
	}
};

class MeterHealthJsonGenerator final :
	public ResultGenerator<MeterHealthResult> {
public:
	int write(const MeterHealthResult &result,
		  std::ostream &output) const override
	{
		write_json_success(output, meter_health_dto(result));
		// A diagnostic response remains a successful machine query even when
		// the observed system is unhealthy.
		return 0;
	}
};

int run_meter_health(const Options &options, std::ostream &output)
{
	AcquisitionClient client(options.socket_path);
	const auto response =
		client.request(options.health_refresh
				       ? AcquisitionCommand::health_refresh
				       : AcquisitionCommand::health,
			       options.timeout_ms);
	require_daemon_ok(response);
	const MeterHealthResult result{
		.response = response,
		.status = evaluate_meter_health(response),
		.full = options.health_full,
	};
	return render_result(options, result, output,
			     MeterHealthTextGenerator{},
			     MeterHealthJsonGenerator{});
}

void print_record(const MeterRecord &record, std::ostream &output)
{
	static constexpr std::array<const char *, 8> names{
		"ILA", "ILB", "ILC", "ILN", "VLC", "VLB", "VLA", "VCM"};
	// CH7/VCM remains present in MTR1 and MeterRecord for future reference
	// monitoring, but it is not a user-facing meter channel yet.
	static constexpr std::size_t displayed_channel_count = 7;
	output << "\033[2J\033[HMSAP1 meter results"
	       << "  sequence=" << record.sequence() << "  generation=0x" << std::hex
	       << record.configuration_generation() << std::dec
	       << "  window=" << record.window_samples() << " samples\n\n";
	for (std::size_t index = 0; index < displayed_channel_count; ++index) {
		const auto channel = record.channel(index);
		output << "CH" << index << ' ' << std::setw(3) << names[index]
		       << "  RMS=";
		if (channel.valid)
			output << std::fixed << std::setprecision(3)
			       << static_cast<double>(channel.rms_micro_units) / 1000000.0
			       << (index >= 4 && index <= 6 ? " V" : " A");
		else
			output << "invalid";
		output << "  mean=" << channel.mean_micro_units
		       << " micro-units  rms_count=" << channel.rms_count << '\n';
	}
	const auto frequency = record.frequency();
	output << "\nFrequency=";
	if (frequency.valid)
		output << std::fixed << std::setprecision(3)
		       << static_cast<double>(frequency.millihz) / 1000.0
		       << " Hz (" << static_cast<unsigned>(frequency.cycles_used)
		       << " cycles)";
	else if (!frequency.enabled)
		output << "disabled";
	else if (frequency.out_of_range)
		output << "unavailable (out of range)";
	else if (frequency.timed_out)
		output << "unavailable (no signal)";
	else
		output << "unavailable (measuring)";
	output << "\nPL capture=" << record.capture_frames()
	       << " header_errors=" << record.header_errors()
	       << " fifo_overflows=" << record.fifo_overflows()
	       << " packetizer_drops=" << record.packetizer_drops()
	       << " hub_drops=" << record.hub_drops() << "\nCtrl-C to stop.\n"
	       << std::flush;
}

int run_meter_view(const Options &options, std::ostream &output)
{
	AcquisitionClient client(options.socket_path);
	const auto started = Clock::now();
	std::optional<std::uint32_t> last_sequence;
	std::uint64_t displayed = 0;
	while (!stop_requested) {
		if (options.duration_seconds &&
		    std::chrono::duration<double>(Clock::now() - started).count() >=
			    *options.duration_seconds)
			break;
		if (options.result_limit && displayed >= *options.result_limit)
			break;
		const auto response =
			client.request(AcquisitionCommand::info, options.timeout_ms);
		require_daemon_ok(response);
		if (response.running == 0u)
			throw std::runtime_error(
				"FPGA acquisition is stopped; run 'mnc adc start'");
		if (response.has_meter_record != 0u &&
		    (!last_sequence || response.latest_record.sequence() != *last_sequence)) {
			if (!response.latest_record.header_valid())
				throw std::runtime_error(
					"daemon returned an invalid meter record");
			print_record(response.latest_record, output);
			last_sequence = response.latest_record.sequence();
			++displayed;
		}
		std::this_thread::sleep_for(50ms);
	}
	return 0;
}

struct MeterChannelDto {
	std::uint32_t channel = 0;
	std::string name;
	std::string quantity;
	bool valid = false;
	std::int64_t mean_micro_units = 0;
	std::uint32_t rms_count = 0;
	std::int64_t rms_micro_units = 0;
};

struct MeterFrequencyDto {
	bool enabled = false;
	bool valid = false;
	bool out_of_range = false;
	bool timed_out = false;
	bool arithmetic_error = false;
	std::uint32_t millihz = 0;
	std::uint32_t period_q16_samples = 0;
	std::uint32_t measurement_sequence = 0;
	std::uint32_t cycles_used = 0;
};

struct MeterSnapshot {
	std::uint32_t sequence = 0;
	std::uint32_t configuration_generation = 0;
	std::uint32_t sample_rate_hz = 0;
	std::uint32_t window_samples = 0;
	std::uint32_t capture_frames = 0;
	std::uint32_t header_errors = 0;
	std::uint32_t fifo_overflows = 0;
	std::uint32_t packetizer_drops = 0;
	std::uint32_t hub_drops = 0;
	std::vector<MeterChannelDto> channels;
	MeterFrequencyDto frequency;
};

MeterSnapshot meter_snapshot(const MeterRecord &record)
{
	static constexpr std::array<const char *, 8> names{
		"ILA", "ILB", "ILC", "ILN", "VLC", "VLB", "VLA", "VCM"};
	MeterSnapshot result{
		.sequence = record.sequence(),
		.configuration_generation = record.configuration_generation(),
		.sample_rate_hz = record.sample_rate_hz(),
		.window_samples = record.window_samples(),
		.capture_frames = record.capture_frames(),
		.header_errors = record.header_errors(),
		.fifo_overflows = record.fifo_overflows(),
		.packetizer_drops = record.packetizer_drops(),
		.hub_drops = record.hub_drops(),
		.channels = {},
		.frequency = {},
	};
	for (std::size_t index = 0; index < 7; ++index) {
		const auto channel = record.channel(index);
		result.channels.push_back({
			.channel = static_cast<std::uint32_t>(index),
			.name = names[index],
			.quantity = index < 4 ? "current" : "voltage",
			.valid = channel.valid,
			.mean_micro_units = channel.mean_micro_units,
			.rms_count = channel.rms_count,
			.rms_micro_units = channel.rms_micro_units,
		});
	}
	const auto frequency = record.frequency();
	result.frequency = {
		.enabled = frequency.enabled,
		.valid = frequency.valid,
		.out_of_range = frequency.out_of_range,
		.timed_out = frequency.timed_out,
		.arithmetic_error = frequency.arithmetic_error,
		.millihz = frequency.millihz,
		.period_q16_samples = frequency.period_q16_samples,
		.measurement_sequence = frequency.measurement_sequence,
		.cycles_used = frequency.cycles_used,
	};
	return result;
}

class SnapshotTextGenerator final : public ResultGenerator<MeterSnapshot> {
public:
	int write(const MeterSnapshot &snapshot,
		  std::ostream &output) const override
	{
		output << "MSAP1 meter snapshot  sequence=" << snapshot.sequence
		       << "  generation=0x" << std::hex
		       << snapshot.configuration_generation << std::dec << '\n';
		for (const auto &channel : snapshot.channels) {
			output << "  CH" << channel.channel << ' ' << std::setw(3)
			       << channel.name << "  ";
			if (!channel.valid)
				output << "unavailable\n";
			else
				output << std::fixed << std::setprecision(3)
				       << static_cast<double>(
						  channel.rms_micro_units) /
						  1000000.0
				       << (channel.quantity == "voltage" ? " V" : " A")
				       << " RMS\n";
		}
		output << "  Frequency: ";
		if (snapshot.frequency.valid)
			output << std::fixed << std::setprecision(3)
			       << static_cast<double>(snapshot.frequency.millihz) /
					  1000.0
			       << " Hz\n";
		else
			output << "unavailable\n";
		return 0;
	}
};

class SnapshotJsonGenerator final : public ResultGenerator<MeterSnapshot> {
public:
	int write(const MeterSnapshot &snapshot,
		  std::ostream &output) const override
	{
		write_json_success(output, snapshot);
		return 0;
	}
};

int run_meter_snapshot(const Options &options, std::ostream &output)
{
	AcquisitionClient client(options.socket_path);
	const auto response =
		client.request(AcquisitionCommand::info, options.timeout_ms);
	require_daemon_ok(response);
	if (response.has_meter_record == 0u)
		throw std::runtime_error("no meter record is available");
	if (!response.latest_record.header_valid())
		throw std::runtime_error("daemon returned an invalid meter record");
	const auto result = meter_snapshot(response.latest_record);
	return render_result(options, result, output, SnapshotTextGenerator{},
			     SnapshotJsonGenerator{});
}

} // namespace

void register_meter_commands(Application &application)
{
	Command meter("meter", "Inspect MSAP1 meter health and readings");
	Command health(
		"health", "Show cached acquisition and meter health",
		run_meter_health,
		{
			.access = AccessLevel::diagnostic,
			.side_effect = SideEffect::none,
			.supports_text = true,
			.supports_json = true,
			.variants = {
				{"--refresh", AccessLevel::maintenance,
				 "Run an immediate RPU register-health audit"},
			},
		});
	health.set_access_resolver([](const Options &options) {
		return options.health_refresh ? AccessLevel::maintenance
					      : AccessLevel::diagnostic;
	});
	health.add_option({
		"refresh", "", "Run an immediate RPU ADC register-health audit",
		CompletionKind::none,
		[](Options &options, const std::string &) {
			options.health_refresh = true;
		},
		false,
	});
	health.add_option({
		"full", "", "Show complete pipeline and AD7771 register diagnostics",
		CompletionKind::none,
		[](Options &options, const std::string &) {
			options.health_full = true;
		},
		false,
	});
	meter.add_subcommand(std::move(health));
	meter.add_subcommand(Command(
		"snapshot", "Read one coherent meter result",
		run_meter_snapshot,
		{
			.access = AccessLevel::diagnostic,
			.side_effect = SideEffect::none,
			.supports_text = true,
			.supports_json = true,
			.variants = {},
		}));
	Command view(
		"view", "Continuously display the latest meter readings",
		run_meter_view,
		{
			.access = AccessLevel::local_only,
			.side_effect = SideEffect::continuous,
			.supports_text = true,
			.supports_json = false,
			.variants = {},
		});
	view.add_option({
		"results", "COUNT", "Stop after displaying COUNT results",
		CompletionKind::none,
		[](Options &options, const std::string &value) {
			options.result_limit = parse_positive_integer(value, "--results");
		},
	});
	view.add_option({
		"duration", "SECONDS", "Stop after the specified duration",
		CompletionKind::none,
		[](Options &options, const std::string &value) {
			options.duration_seconds = parse_duration(value);
		},
	});
	meter.add_subcommand(std::move(view));
	application.add_command(std::move(meter));
}

void request_stop() noexcept { stop_requested = 1; }
bool stop_was_requested() noexcept { return stop_requested != 0; }

} // namespace msap1::cli
