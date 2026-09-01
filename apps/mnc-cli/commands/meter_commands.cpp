#include "core/cli.hpp"
#include "core/result_output.hpp"

#include "msap1/acquisition/ipc/acquisition_ipc.hpp"
#include "msap1/meter/MeterDataProvider/stream/meter_stream_ipc.hpp"
#include "msap1/meter/history/historian_ipc.hpp"
#include "msap1/meter/meter_health.hpp"
#include "msap1/settings/settings_ipc.hpp"
#include "msap1/waveform/mncwf_v4.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cctype>
#include <csignal>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <optional>
#include <pwd.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

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

std::int64_t parse_signed_integer(const std::string &value,
	const std::string &option)
{
	std::size_t end = 0;
	std::int64_t result = 0;
	try {
		result = std::stoll(value, &end, 10);
	} catch (const std::exception &) {
		throw std::invalid_argument(option +
			" requires signed decimal nanoseconds");
	}
	if (end != value.size())
		throw std::invalid_argument(option +
			" requires signed decimal nanoseconds");
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

std::array<CurrentWiringChannelConfig *, 4> wiring_channels(
	CurrentWiringConfig &configuration)
{
	return {&configuration.channels.ch0, &configuration.channels.ch1,
		&configuration.channels.ch2, &configuration.channels.ch3};
}

std::array<const CurrentWiringChannelConfig *, 4> wiring_channels(
	const CurrentWiringConfig &configuration)
{
	return {&configuration.channels.ch0, &configuration.channels.ch1,
		&configuration.channels.ch2, &configuration.channels.ch3};
}

void require_daemon_ok(AcquisitionStatus status);

CurrentWiringConfig unpack_current_wiring(
	std::uint32_t phase_map, std::uint32_t invert_mask,
	std::string input_order = {})
{
	static constexpr std::array phases{"A", "B", "C", "N"};
	CurrentWiringConfig result;
	if (input_order.empty())
		input_order = phase_map == 0xe4u ? "ABC" :
			phase_map == 0xd8u ? "ACB" : "CUSTOM";
	result.input_order = std::move(input_order);
	const auto channels = wiring_channels(result);
	for (std::size_t channel = 0; channel < channels.size(); ++channel) {
		channels[channel]->phase =
			phases[(phase_map >> (channel * 2u)) & 0x3u];
		channels[channel]->direction =
			(invert_mask & (1u << channel)) != 0u ?
				"reversed" : "normal";
	}
	return result;
}

std::string wiring_apply_result(std::uint32_t value)
{
	switch (value) {
	case MSAP1_METER_WIRING_APPLY_NONE: return "none";
	case MSAP1_METER_WIRING_APPLY_SUCCESS: return "success";
	case MSAP1_METER_WIRING_APPLY_FAILED: return "failed";
	case MSAP1_METER_WIRING_APPLY_ROLLED_BACK: return "rolled_back";
	case MSAP1_METER_WIRING_APPLY_ROLLBACK_FAILED:
		return "rollback_failed";
	default: return "unknown";
	}
}

struct CurrentWiringStatus {
	CurrentWiringConfig requested;
	CurrentWiringConfig active;
	std::uint32_t generation = 0;
	bool match = false;
	std::string last_apply_result;
	std::uint32_t readback_mismatch_count = 0;
};

CurrentWiringStatus current_wiring_status(const Options &options)
{
	settings::ipc::SettingsClient settings_client;
	const auto settings = settings_client.active(options.timeout_ms);
	AcquisitionClient acquisition(options.socket_path);
	const auto response = acquisition.request(HealthRequest{}, options.timeout_ms);
	require_daemon_ok(response.status);
	const auto health = response.rpu_health.value();
	const auto match =
		(health.meter_health_flags &
		 MSAP1_METER_HEALTH_CURRENT_WIRING_MATCH) != 0u &&
		health.meter_active_current_adc_phase_map ==
			current_adc_phase_map(settings.metering.current_wiring) &&
		health.meter_active_current_adc_invert_mask ==
			current_adc_invert_mask(settings.metering.current_wiring);
	return {
		settings.metering.current_wiring,
		unpack_current_wiring(
			health.meter_active_current_adc_phase_map,
			health.meter_active_current_adc_invert_mask,
			match ? settings.metering.current_wiring.input_order :
				std::string{}),
		health.meter_generation,
		match,
		wiring_apply_result(health.meter_wiring_apply_status),
		health.meter_wiring_readback_mismatch_count,
	};
}

void write_current_wiring_text(const CurrentWiringStatus &status,
	std::ostream &output)
{
	const auto requested = wiring_channels(status.requested);
	const auto active = wiring_channels(status.active);
	output << "ADC current wiring\n"
	       << "  Requested preset:  " << status.requested.input_order << '\n'
	       << "  Active preset:     " << status.active.input_order << '\n'
	       << "  Generation:        " << status.generation << '\n'
	       << "  Match:             " << (status.match ? "yes" : "NO") << '\n'
	       << "  Last apply:        " << status.last_apply_result << '\n'
	       << "  Readback mismatches: " << status.readback_mismatch_count << '\n';
	for (std::size_t channel = 0; channel < requested.size(); ++channel)
		output << "  CH" << channel << ": requested "
		       << requested[channel]->phase << "/"
		       << requested[channel]->direction << ", active "
		       << active[channel]->phase << "/"
		       << active[channel]->direction << '\n';
}

int run_meter_wiring_show(const Options &options, std::ostream &output)
{
	const auto status = current_wiring_status(options);
	if (options.output_format == OutputFormat::json)
		write_json_success(output, status);
	else
		write_current_wiring_text(status, output);
	return 0;
}

int run_meter_wiring_set(const Options &options, std::ostream &output)
{
	const bool has_explicit_phase = std::ranges::any_of(
		options.current_channel_phase,
		[](const auto &value) { return value.has_value(); });
	const bool has_direction = std::ranges::any_of(
		options.current_channel_direction,
		[](const auto &value) { return value.has_value(); });
	if (options.current_wiring_preset && has_explicit_phase)
		throw std::invalid_argument(
			"--preset cannot be combined with explicit phase assignments");
	if (!options.current_wiring_preset && !has_explicit_phase && !has_direction)
		throw std::invalid_argument(
			"wiring set requires a preset, phase, or direction option");

	settings::ipc::SettingsClient client;
	auto settings = client.active(options.timeout_ms);
	auto &wiring = settings.metering.current_wiring;
	if (options.current_wiring_preset) {
		if (*options.current_wiring_preset == "abc") {
			wiring.input_order = "ABC";
			wiring.channels.ch0.phase = "A";
			wiring.channels.ch1.phase = "B";
			wiring.channels.ch2.phase = "C";
			wiring.channels.ch3.phase = "N";
		} else if (*options.current_wiring_preset == "acb") {
			wiring.input_order = "ACB";
			wiring.channels.ch0.phase = "A";
			wiring.channels.ch1.phase = "C";
			wiring.channels.ch2.phase = "B";
			wiring.channels.ch3.phase = "N";
		} else {
			throw std::invalid_argument("--preset must be abc or acb");
		}
	}
	const auto channels = wiring_channels(wiring);
	for (std::size_t channel = 0; channel < channels.size(); ++channel) {
		if (options.current_channel_phase[channel]) {
			const auto &phase = *options.current_channel_phase[channel];
			if (phase != "a" && phase != "b" && phase != "c" &&
			    phase != "n")
				throw std::invalid_argument(
					"channel phase must be a, b, c, or n");
			channels[channel]->phase = std::string(1,
				static_cast<char>(std::toupper(
					static_cast<unsigned char>(phase.front()))));
			wiring.input_order = "CUSTOM";
		}
		if (options.current_channel_direction[channel]) {
			const auto &direction =
				*options.current_channel_direction[channel];
			if (direction != "normal" && direction != "reversed")
				throw std::invalid_argument(
					"channel direction must be normal or reversed");
			channels[channel]->direction = direction;
		}
	}
	validate_current_wiring(wiring);
	settings::ipc::Request request;
	request.command = settings::ipc::Command::save_active;
	request.json = settings::SettingsCodec::encode(settings, false);
	const auto response = client.request(
		std::move(request), options.timeout_overridden ? options.timeout_ms : 35000);
	if (response.status != settings::ipc::Status::ok)
		throw std::runtime_error(response.message.empty()
			? "settings service rejected current wiring" : response.message);
	return run_meter_wiring_show(options, output);
}

OptionSpec current_wiring_option(std::string name, std::string summary,
	std::size_t channel, bool direction)
{
	return {std::move(name), direction ? "normal|reversed" : "a|b|c|n",
		std::move(summary), CompletionKind::none,
		[channel, direction](Options &options, const std::string &value) {
			if (direction) {
				if (value != "normal" && value != "reversed")
					throw std::invalid_argument(
						"channel direction must be normal or reversed");
				options.current_channel_direction[channel] = value;
			} else {
				if (value != "a" && value != "b" && value != "c" &&
				    value != "n")
					throw std::invalid_argument(
						"channel phase must be a, b, c, or n");
				options.current_channel_phase[channel] = value;
			}
		}};
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

void require_daemon_ok(AcquisitionStatus status)
{
	if (status != AcquisitionStatus::ok)
		throw std::runtime_error("acquisition daemon request failed (status " +
			std::to_string(static_cast<std::uint32_t>(status)) + ")");
}

struct MeterHealthResult {
	InfoResponse response;
	MeterHealth status;
	bool full = false;
};

/*
 * Kernel DMA transport counters as one value line, shared by the short and
 * full reports so the two can never drift apart. The deficit is the derived
 * number worth reading: the Xilinx cyclic callback fires per interrupt, not
 * per period, so it counts the period completions that were coalesced into a
 * neighbouring interrupt. Diagnostic only — nothing depends on it.
 */
std::string transport_counters(const InfoResponse &response)
{
	return "produced " + std::to_string(response.transport_produced_blocks) +
	       ", consumed " + std::to_string(response.transport_consumed_blocks) +
	       ", overruns " + std::to_string(response.transport_overrun_blocks) +
	       ", callbacks " + std::to_string(response.transport_callbacks) +
	       " (deficit " +
	       std::to_string(transport_callback_deficit(response)) +
	       "), ring " + std::to_string(response.transport_ring_blocks);
}

int write_meter_health_text(const MeterHealthResult &result,
			    std::ostream &output)
{
	const auto &response = result.response;
	const auto &status = result.status;
	const auto health = response.rpu_health.value();

	if (!result.full) {
		output << "MSAP1 meter health: "
		       << (status.healthy ? "PASS" : "FAIL") << '\n'
		       << "  Acquisition:         "
		       << (status.acquisition_healthy ? "healthy" : "degraded")
		       << " (" << (response.running ? "running" : "stopped")
		       << ", record ";
		if (response.meter_record_age_ms !=
		    std::numeric_limits<std::uint32_t>::max())
			output << response.meter_record_age_ms << " ms old)\n";
		else
			output << "unavailable)\n";
		output << "  ADC:                 "
		       << (status.adc_healthy ? "healthy" : "degraded") << '\n'
		       << "  RPU aggregation:     ";
		if (!status.aggregation_health_available) {
			output << "unavailable";
			if (response.aggregation_health_probe_pending)
				output << " (probe pending, "
				       << response.aggregation_health_probe_failures
				       << " failure(s))";
			output << '\n';
		} else {
			output << (status.aggregation_healthy ? "healthy" : "degraded")
			       << " ("
			       << (status.aggregation_authoritative ? "authoritative"
							       : "shadow")
			       << ")\n";
		}
		output
		       << "  Sample rate:         " << health.sample_rate_hz
		       << " configured / ";
		if (health.drdy_frequency_hz != 0u)
			output << health.drdy_frequency_hz;
		else
			output << "unavailable";
		output << " measured frame/s\n"
		       << "  Data-path errors:    DMA " << response.dma_read_errors
		       << ", invalid " << response.invalid_records << " epoch/"
		       << response.lifetime_invalid_records << " lifetime, gaps "
		       << response.sequence_gaps << ", FIFO "
		       << health.overflow_count << ", headers "
		       << health.header_error_count << '\n'
		       << "  DMA transport:       "
		       << transport_counters(response) << '\n'
		       << "  Health audit:        ";
		if (response.health_probe_pending) {
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
		if (!status.aggregation_degraded_reasons.empty()) {
			output << "  RPU aggregation degraded because:\n";
			for (const auto &reason :
			     status.aggregation_degraded_reasons)
				output << "    - [" << reason.code << "] "
				       << reason.message << '\n';
		}
		output << "  Run 'mnc meter health --full' for complete diagnostics.\n";
		return status.healthy ? 0 : 1;
	}

	output << "MSAP1 meter health: " << (status.healthy ? "PASS" : "FAIL") << '\n'
	       << "  Linux acquisition:    " << yes_no(response.running) << '\n'
	       << "  Meter record present: " << yes_no(response.has_meter_record)
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
	output << "  R5C1 aggregation:     ";
	if (!status.aggregation_health_available) {
		output << "unavailable";
		if (response.aggregation_health_probe_pending)
			output << " (probe pending, "
			       << response.aggregation_health_probe_failures
			       << " failure(s))";
		output << '\n';
	} else {
		output << (status.aggregation_healthy ? "healthy" : "degraded")
		       << " ("
		       << (status.aggregation_authoritative ? "authoritative"
						       : "shadow")
		       << ")\n";
		output << "  R5C1 health age:      ";
		if (response.aggregation_health_age_ms !=
		    std::numeric_limits<std::uint32_t>::max())
			output << response.aggregation_health_age_ms << " ms\n";
		else
			output << "unavailable\n";
		output << "  R5C1 RPMsg device:    "
		       << (response.aggregation_rpmsg_device.empty()
				   ? "unavailable"
				   : response.aggregation_rpmsg_device)
		       << '\n';
		const auto aggregation = response.rpu_aggregation_health.value();
		output << "  R5C1 input frames:    " << aggregation.frames_received
		       << " received, " << aggregation.frames_valid << " valid, "
		       << aggregation.frames_invalid << " invalid\n"
		       << "  R5C1 input sequence:  "
		       << aggregation.last_input_sequence << " last, "
		       << aggregation.expected_input_sequence << " expected, "
		       << aggregation.sequence_gaps << " gap(s), "
		       << aggregation.input_records_dropped
		       << " inferred drop(s)\n"
		       << "  R5C1 input errors:    CRC " << aggregation.crc_errors
		       << ", format " << aggregation.format_errors << ", length "
		       << aggregation.length_errors << ", FIFO "
		       << aggregation.fifo_errors << ", ring "
		       << aggregation.ring_overflows << ", ring push "
		       << aggregation.software_ring_push_failures << '\n';
		if (aggregation.input_records_dropped != 0u)
			output << "  R5C1 dropped range:   "
			       << aggregation.first_dropped_sequence << " first, "
			       << aggregation.last_dropped_sequence << " last\n";
		output << "  R5C1 output records:  " << aggregation.records_queued
		       << " queued, " << aggregation.records_emitted << " emitted, "
		       << aggregation.output_errors << " error(s), "
		       << aggregation.output_drops << " drop(s)\n"
		       << "  R5C1 completions:     basic "
		       << aggregation.basic_completed << ", 150/180 "
		       << aggregation.aggregate_completed << ", 10-minute "
		       << aggregation.ten_minute_completed << ", 2-hour "
		       << aggregation.two_hour_completed << '\n'
		       << "  R5C1 software ring:   "
		       << aggregation.software_ring_current << '/'
		       << aggregation.software_ring_capacity << " current, "
		       << aggregation.software_ring_high_water
		       << " high-water, pressure "
		       << aggregation.software_ring_pressure << '\n'
		       << "  R5C1 pressure edges:  warning "
		       << aggregation.software_ring_warning_entries << ", high "
		       << aggregation.software_ring_high_entries << ", critical "
		       << aggregation.software_ring_critical_entries << ", full "
		       << aggregation.software_ring_full_entries << '\n'
		       << "  R5C1 hardware FIFO:   "
		       << aggregation.hardware_fifo_current_words
		       << " current words, "
		       << aggregation.hardware_fifo_high_water_words
		       << " high-water, programmable-full edges "
		       << aggregation.hardware_fifo_full_events << '\n'
		       << "  R5C1 input worker:    "
		       << aggregation.input_wake_count << " wakes, "
		       << aggregation.input_records_processed << " records, max batch "
		       << aggregation.input_max_batch << ", max runtime "
		       << aggregation.input_max_runtime_us << " us\n"
		       << "  R5C1 validator:       "
		       << aggregation.validator_wake_count << " wakes, "
		       << aggregation.validator_records_processed
		       << " records, max runtime "
		       << aggregation.validator_max_runtime_us
		       << " us, max schedule gap "
		       << aggregation.validator_max_schedule_gap_us << " us\n"
		       << "  R5C1 stack headroom:  control/input/output/validator "
		       << aggregation.control_stack_high_water_bytes << '/'
		       << aggregation.input_stack_high_water_bytes << '/'
		       << aggregation.output_stack_high_water_bytes << '/'
		       << aggregation.validator_stack_high_water_bytes << " bytes\n";
	}
	output << "  Health confirmation:  "
	       << (!response.health_probe_pending
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
	       << "  Invalid records:      " << response.invalid_records
	       << " current epoch\n"
	       << "  Invalid records total: " << response.lifetime_invalid_records
	       << " process lifetime\n"
	       << "  Sequence gaps:        " << response.sequence_gaps << '\n'
	       << "  DMA transport:        " << transport_counters(response)
	       << '\n'
	       << "  Configuration gen:    0x" << std::hex
	       << response.configuration_generation << std::dec << '\n'
	       << "  PL generation match:  "
	       << yes_no(status.meter_generation_match) << '\n'
	       << "  Meter configured:     " << yes_no(status.meter_configured) << '\n'
	       << "  DC offset removal:    " << yes_no(status.dc_offset_removal) << '\n'
	       << "  ADC SPI responsive:   ";
	if (status.physical_diagnostics_applicable)
		output << yes_no(status.spi_responsive) << '\n';
	else
		output << "not applicable (simulator)\n";
	output
	       << "  ADC rate match:       " << yes_no(status.rate_match) << '\n'
	       << "  Capture active:       " << yes_no(status.capture_active) << '\n'
	       << "  Sample rate:          " << health.sample_rate_hz << " frame/s\n"
	       << "  PL frames:            " << health.frame_count << '\n'
	       << "  ADC packets:          " << health.packet_count << '\n'
	       << "  ADC DCLK:             ";
	if (!status.physical_diagnostics_applicable)
		output << "not applicable (simulator)\n";
	else if (health.dclk_frequency_hz != 0u)
		output << health.dclk_frequency_hz << " Hz\n";
	else
		output << "unavailable\n";
	output << "  ADC DRDY:             ";
	if (health.drdy_frequency_hz != 0u)
		output << health.drdy_frequency_hz << " frame/s\n";
	else
		output << "unavailable\n";
	output << "  FIFO overflows:       " << health.overflow_count << '\n'
	       << "  Header errors:        " << health.header_error_count << '\n';
	if (status.physical_diagnostics_applicable) {
		output << "  SPI protocol errors:  "
		       << health.spi_protocol_error_count << '\n'
		       << "  SPI retry recoveries: "
		       << health.spi_retry_recovery_count << '\n'
		       << "  Config mismatches:    "
		       << health.spi_config_read_mismatch_count << '\n'
		       << "  GEN_ERR_REG_1 events: "
		       << health.spi_general_error_1_events << '\n'
		       << "  Last SPI failure:     register 0x" << std::hex
		       << static_cast<unsigned int>(health.spi_last_failed_register)
		       << ", header 0x"
		       << static_cast<unsigned int>(health.spi_last_received_header)
		       << std::dec << '\n';
	}
	/* Only the shape of the bad-header distribution distinguishes a
	 * systematic corruption from random mis-sampling, so print the
	 * populated buckets rather than a single most-recent sample.
	 * Silent when the bus is clean. */
	if (status.physical_diagnostics_applicable) {
		std::ostringstream buckets;
		for (std::size_t bucket = 0; bucket < 16; ++bucket) {
			if (health.spi_header_histogram[bucket] == 0)
				continue;
			if (!buckets.str().empty())
				buckets << "  ";
			buckets << "0x" << std::hex << bucket << "_" << std::dec
				<< "=" << health.spi_header_histogram[bucket];
		}
		if (!buckets.str().empty())
			output << "  Bad header buckets:   " << buckets.str()
			       << '\n';
	}
	/* GEN_ERR_REG_1 is clear-on-read, so the sweep that samples it also
	 * destroys it. Name whichever bits were ever seen; silent if none. */
	if (status.physical_diagnostics_applicable &&
	    health.spi_general_error_1_sticky != 0u) {
		static constexpr std::array<const char *, 8> gen_err_1_bits{
			nullptr, "SPI_CRC_ERR", "SPI_INVALID_WRITE_ERR",
			"SPI_INVALID_READ_ERR", "SPI_CLK_COUNT_ERR",
			"ROM_CRC_ERR", "MEMMAP_CRC_ERR", nullptr};
		std::ostringstream bits;
		for (std::size_t bit = 0; bit < gen_err_1_bits.size(); ++bit) {
			if (gen_err_1_bits[bit] == nullptr ||
			    (health.spi_general_error_1_sticky &
			     (1u << bit)) == 0u)
				continue;
			if (!bits.str().empty())
				bits << ", ";
			bits << gen_err_1_bits[bit];
		}
		if (bits.str().empty())
			bits << "reserved bits only";
		output << "  ADC latched SPI errs: " << bits.str() << '\n';
	}
	output
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
	if (!status.aggregation_degraded_reasons.empty()) {
		output << "  RPU aggregation degraded because:\n";
		for (const auto &reason : status.aggregation_degraded_reasons)
			output << "    - [" << reason.code << "] "
			       << reason.message << '\n';
	}
	if (!status.physical_diagnostics_applicable)
		output << "\n  AD7771 register snapshot: not applicable (simulator)\n";
	else if (status.spi_responsive)
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

/* Machine form of the "DMA transport" text line, including the derived
 * callback deficit so a consumer never has to re-derive it. */
struct TransportHealthDto {
	std::uint64_t produced_blocks = 0;
	std::uint64_t consumed_blocks = 0;
	std::uint64_t overrun_blocks = 0;
	std::uint64_t callbacks = 0;
	std::uint64_t callback_deficit = 0;
	std::uint32_t ring_blocks = 0;
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
	std::uint64_t lifetime_invalid_records = 0;
	std::uint64_t sequence_gaps = 0;
	TransportHealthDto dma_transport;
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
	std::uint32_t spi_protocol_errors = 0;
	std::uint32_t spi_retry_recoveries = 0;
	/* Configuration reads whose two samples disagreed: the data-byte
	 * corruption the protocol header check cannot detect. */
	std::uint32_t spi_config_mismatches = 0;
	/* Sticky OR of GEN_ERR_REG_1, and how many polls saw it non-zero.
	 * The register clears on read, so this is the only lasting record
	 * of the ADC's own view of an SPI fault. */
	std::uint32_t spi_general_error_1_sticky = 0;
	std::uint32_t spi_general_error_1_events = 0;
	/* Malformed reply headers bucketed by high nibble; index is the
	 * nibble. All-zero on a healthy bus (the only valid header is
	 * 0x20). Exported as a fixed 16-slot array so a consumer can
	 * difference two samples without matching up sparse keys. */
	std::array<std::uint16_t, 16> spi_header_histogram{};
	std::vector<HealthReasonDto> degraded_reasons;
	std::vector<RegisterDto> registers;
};

struct AggregationHealthDto {
	bool available = false;
	bool healthy = false;
	bool authoritative = false;
	bool transport_available = false;
	bool transport_initialized = false;
	bool input_healthy = false;
	bool engine_ready = false;
	bool output_ready = false;
	bool output_active = false;
	bool probe_pending = false;
	std::uint32_t probe_failures = 0;
	std::optional<std::uint32_t> cache_age_ms;
	std::string rpmsg_device;
	std::uint32_t health_flags = 0;
	std::uint32_t frames_received = 0;
	std::uint32_t frames_valid = 0;
	std::uint32_t frames_invalid = 0;
	std::uint32_t crc_errors = 0;
	std::uint32_t format_errors = 0;
	std::uint32_t sequence_gaps = 0;
	std::uint32_t repeated_frames = 0;
	std::uint32_t out_of_order_frames = 0;
	std::uint32_t ring_overflows = 0;
	std::uint32_t software_ring_push_failures = 0;
	std::uint32_t input_records_dropped = 0;
	std::uint32_t first_dropped_sequence = 0;
	std::uint32_t last_dropped_sequence = 0;
	std::uint32_t fifo_errors = 0;
	std::uint32_t length_errors = 0;
	std::uint32_t records_queued = 0;
	std::uint32_t records_emitted = 0;
	std::uint32_t output_errors = 0;
	std::uint32_t output_drops = 0;
	std::uint32_t basic_completed = 0;
	std::uint32_t aggregate_completed = 0;
	std::uint32_t ten_minute_completed = 0;
	std::uint32_t two_hour_completed = 0;
	std::uint32_t last_input_sequence = 0;
	std::uint32_t expected_input_sequence = 0;
	std::uint32_t last_output_sequence = 0;
	std::uint32_t last_fifo_error = 0;
	std::uint32_t last_frame_length = 0;
	std::uint32_t last_validation_error = 0;
	std::uint32_t last_tx_vacancy = 0;
	std::uint32_t software_ring_current = 0;
	std::uint32_t software_ring_high_water = 0;
	std::uint32_t software_ring_capacity = 0;
	std::uint32_t software_ring_pressure = 0;
	std::uint32_t software_ring_warning_entries = 0;
	std::uint32_t software_ring_high_entries = 0;
	std::uint32_t software_ring_critical_entries = 0;
	std::uint32_t software_ring_full_entries = 0;
	std::uint32_t hardware_fifo_current_words = 0;
	std::uint32_t hardware_fifo_high_water_words = 0;
	std::uint32_t hardware_fifo_full_events = 0;
	std::uint32_t input_wake_count = 0;
	std::uint32_t input_records_processed = 0;
	std::uint32_t input_max_batch = 0;
	std::uint32_t input_max_runtime_us = 0;
	std::uint32_t validator_wake_count = 0;
	std::uint32_t validator_records_processed = 0;
	std::uint32_t validator_max_runtime_us = 0;
	std::uint32_t validator_max_schedule_gap_us = 0;
	std::uint32_t control_stack_high_water_bytes = 0;
	std::uint32_t input_stack_high_water_bytes = 0;
	std::uint32_t output_stack_high_water_bytes = 0;
	std::uint32_t validator_stack_high_water_bytes = 0;
	std::vector<HealthReasonDto> degraded_reasons;
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
	AggregationHealthDto aggregation;
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
	const auto health = response.rpu_health.value();
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
		.health_probe_pending = response.health_probe_pending,
		.health_probe_failures = response.health_probe_failures,
		.acquisition = {
			.healthy = status.acquisition_healthy,
			.running = response.running,
			.record_present = response.has_meter_record,
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
			.lifetime_invalid_records =
				response.lifetime_invalid_records,
			.sequence_gaps = response.sequence_gaps,
			.dma_transport = {
				.produced_blocks =
					response.transport_produced_blocks,
				.consumed_blocks =
					response.transport_consumed_blocks,
				.overrun_blocks =
					response.transport_overrun_blocks,
				.callbacks = response.transport_callbacks,
				.callback_deficit =
					transport_callback_deficit(response),
				.ring_blocks = response.transport_ring_blocks,
			},
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
			.spi_protocol_errors = health.spi_protocol_error_count,
			.spi_retry_recoveries = health.spi_retry_recovery_count,
			.spi_config_mismatches =
				health.spi_config_read_mismatch_count,
			.spi_general_error_1_sticky =
				health.spi_general_error_1_sticky,
			.spi_general_error_1_events =
				health.spi_general_error_1_events,
			.degraded_reasons = {},
			.registers = {},
		},
		.aggregation = {
			.available = status.aggregation_health_available,
			.healthy = status.aggregation_healthy,
			.authoritative = status.aggregation_authoritative,
			.transport_available =
				status.aggregation_transport_available,
			.transport_initialized =
				status.aggregation_transport_initialized,
			.input_healthy = status.aggregation_input_healthy,
			.engine_ready = status.aggregation_engine_ready,
			.output_ready = status.aggregation_output_ready,
			.output_active = status.aggregation_output_active,
			.probe_pending = response.aggregation_health_probe_pending,
			.probe_failures =
				response.aggregation_health_probe_failures,
			.cache_age_ms =
				response.aggregation_health_age_ms ==
						std::numeric_limits<std::uint32_t>::max()
					? std::nullopt
					: std::optional<std::uint32_t>(
						  response.aggregation_health_age_ms),
			.rpmsg_device = response.aggregation_rpmsg_device,
			.degraded_reasons = {},
		},
	};
	for (std::size_t bucket = 0;
	     bucket < dto.adc.spi_header_histogram.size(); ++bucket)
		dto.adc.spi_header_histogram[bucket] =
			health.spi_header_histogram[bucket];
	for (const auto &reason : status.adc_degraded_reasons)
		dto.adc.degraded_reasons.push_back({reason.code, reason.message});
	for (const auto &reason : status.aggregation_degraded_reasons)
		dto.aggregation.degraded_reasons.push_back(
			{reason.code, reason.message});
	if (response.has_aggregation_health) {
		const auto aggregation = response.rpu_aggregation_health.value();
		dto.aggregation.health_flags = aggregation.health_flags;
		dto.aggregation.frames_received = aggregation.frames_received;
		dto.aggregation.frames_valid = aggregation.frames_valid;
		dto.aggregation.frames_invalid = aggregation.frames_invalid;
		dto.aggregation.crc_errors = aggregation.crc_errors;
		dto.aggregation.format_errors = aggregation.format_errors;
		dto.aggregation.sequence_gaps = aggregation.sequence_gaps;
		dto.aggregation.repeated_frames = aggregation.repeated_frames;
		dto.aggregation.out_of_order_frames =
			aggregation.out_of_order_frames;
		dto.aggregation.ring_overflows = aggregation.ring_overflows;
		dto.aggregation.software_ring_push_failures =
			aggregation.software_ring_push_failures;
		dto.aggregation.input_records_dropped =
			aggregation.input_records_dropped;
		dto.aggregation.first_dropped_sequence =
			aggregation.first_dropped_sequence;
		dto.aggregation.last_dropped_sequence =
			aggregation.last_dropped_sequence;
		dto.aggregation.fifo_errors = aggregation.fifo_errors;
		dto.aggregation.length_errors = aggregation.length_errors;
		dto.aggregation.records_queued = aggregation.records_queued;
		dto.aggregation.records_emitted = aggregation.records_emitted;
		dto.aggregation.output_errors = aggregation.output_errors;
		dto.aggregation.output_drops = aggregation.output_drops;
		dto.aggregation.basic_completed = aggregation.basic_completed;
		dto.aggregation.aggregate_completed =
			aggregation.aggregate_completed;
		dto.aggregation.ten_minute_completed =
			aggregation.ten_minute_completed;
		dto.aggregation.two_hour_completed =
			aggregation.two_hour_completed;
		dto.aggregation.last_input_sequence =
			aggregation.last_input_sequence;
		dto.aggregation.expected_input_sequence =
			aggregation.expected_input_sequence;
		dto.aggregation.last_output_sequence =
			aggregation.last_output_sequence;
		dto.aggregation.last_fifo_error = aggregation.last_fifo_error;
		dto.aggregation.last_frame_length = aggregation.last_frame_length;
		dto.aggregation.last_validation_error =
			aggregation.last_validation_error;
		dto.aggregation.last_tx_vacancy = aggregation.last_tx_vacancy;
		dto.aggregation.software_ring_current =
			aggregation.software_ring_current;
		dto.aggregation.software_ring_high_water =
			aggregation.software_ring_high_water;
		dto.aggregation.software_ring_capacity =
			aggregation.software_ring_capacity;
		dto.aggregation.software_ring_pressure =
			aggregation.software_ring_pressure;
		dto.aggregation.software_ring_warning_entries =
			aggregation.software_ring_warning_entries;
		dto.aggregation.software_ring_high_entries =
			aggregation.software_ring_high_entries;
		dto.aggregation.software_ring_critical_entries =
			aggregation.software_ring_critical_entries;
		dto.aggregation.software_ring_full_entries =
			aggregation.software_ring_full_entries;
		dto.aggregation.hardware_fifo_current_words =
			aggregation.hardware_fifo_current_words;
		dto.aggregation.hardware_fifo_high_water_words =
			aggregation.hardware_fifo_high_water_words;
		dto.aggregation.hardware_fifo_full_events =
			aggregation.hardware_fifo_full_events;
		dto.aggregation.input_wake_count = aggregation.input_wake_count;
		dto.aggregation.input_records_processed =
			aggregation.input_records_processed;
		dto.aggregation.input_max_batch = aggregation.input_max_batch;
		dto.aggregation.input_max_runtime_us =
			aggregation.input_max_runtime_us;
		dto.aggregation.validator_wake_count =
			aggregation.validator_wake_count;
		dto.aggregation.validator_records_processed =
			aggregation.validator_records_processed;
		dto.aggregation.validator_max_runtime_us =
			aggregation.validator_max_runtime_us;
		dto.aggregation.validator_max_schedule_gap_us =
			aggregation.validator_max_schedule_gap_us;
		dto.aggregation.control_stack_high_water_bytes =
			aggregation.control_stack_high_water_bytes;
		dto.aggregation.input_stack_high_water_bytes =
			aggregation.input_stack_high_water_bytes;
		dto.aggregation.output_stack_high_water_bytes =
			aggregation.output_stack_high_water_bytes;
		dto.aggregation.validator_stack_high_water_bytes =
			aggregation.validator_stack_high_water_bytes;
	}
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
	const auto response = options.health_refresh
		? client.request(HealthRefreshRequest{}, options.timeout_ms)
		: client.request(HealthRequest{}, options.timeout_ms);
	require_daemon_ok(response.status);
	const MeterHealthResult result{
		.response = response,
		.status = evaluate_meter_health(response),
		.full = options.health_full,
	};
	return render_result(options, result, output,
			     MeterHealthTextGenerator{},
			     MeterHealthJsonGenerator{});
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
	std::uint64_t sequence = 0;
	std::uint32_t configuration_generation = 0;
	std::uint32_t sample_rate_hz = 0;
	std::uint32_t block_sample_count = 0;
	std::uint32_t capture_frames = 0;
	std::uint32_t header_errors = 0;
	std::uint32_t fifo_overflows = 0;
	std::uint32_t emit_drops = 0;
	std::uint32_t result_drops = 0;
	std::vector<MeterChannelDto> channels;
	MeterFrequencyDto frequency;
	/* Non-channel attributes (VLL, power, PF, ...): engineering value
	 * plus the catalog's stable key, printed generically so new PL
	 * quantities appear without new plumbing. */
	struct Extra {
		std::string key;
		std::string unit;
		bool valid = false;
		double value = 0.0;
	};
	std::vector<Extra> readings;
};

MeterSnapshot meter_snapshot(const msap1::MeterSnapshotResponse &response)
{
	static constexpr std::array<const char *, 8> names{
		"ILA", "ILB", "ILC", "ILN", "VLC", "VLB", "VLA", "VCM"};
	if (!response.has_snapshot)
		throw std::runtime_error("no meter snapshot is available");
	const auto &snapshot = response.snapshot;
	const auto &diagnostics = response.diagnostics;
	MeterSnapshot result{
		.sequence = snapshot.sequence,
		.configuration_generation = snapshot.configuration_generation,
		.sample_rate_hz = diagnostics.sample_rate_hz,
		.block_sample_count = diagnostics.block_sample_count,
		.capture_frames = diagnostics.capture_frames,
		.header_errors = diagnostics.header_errors,
		.fifo_overflows = diagnostics.fifo_overflows,
		.emit_drops = diagnostics.emit_drops,
		.result_drops = diagnostics.result_drops,
		.channels = {},
		.frequency = {},
		.readings = {},
	};
	for (std::size_t index = 0; index < 7; ++index) {
		result.channels.push_back({
			.channel = static_cast<std::uint32_t>(index),
			.name = names[index],
			.quantity = index < 4 ? "current" : "voltage",
			.valid = false,
			.mean_micro_units = diagnostics.channels[index].mean_micro_units,
			.rms_count = diagnostics.channels[index].rms_count,
			.rms_micro_units = 0,
		});
	}
	result.frequency = {
		.enabled = diagnostics.frequency.enabled,
		.valid = false,
		.out_of_range = diagnostics.frequency.out_of_range,
		.timed_out = diagnostics.frequency.timed_out,
		.arithmetic_error = diagnostics.frequency.arithmetic_error,
		.millihz = 0,
		.period_q16_samples = diagnostics.frequency.period_q16_samples,
		.measurement_sequence = diagnostics.frequency.measurement_sequence,
		.cycles_used = diagnostics.frequency.cycles_used,
	};
	using Id = mnc::meter::MeterAttributeId;
	auto channel_index = [](Id id) -> std::optional<std::size_t> {
		switch (id) {
		case Id::IaRms: return 0;
		case Id::IbRms: return 1;
		case Id::IcRms: return 2;
		case Id::InRms: return 3;
		case Id::VcnRms: return 4;
		case Id::VbnRms: return 5;
		case Id::VanRms: return 6;
		default: return std::nullopt;
		}
	};
	for (const auto &reading : snapshot.values) {
		const bool valid = reading.quality ==
			mnc::meter::ReadingQuality::Valid;
		if (reading.attribute.id == Id::Frequency) {
			result.frequency.valid = valid;
			result.frequency.millihz = valid
				? static_cast<std::uint32_t>(reading.value) : 0u;
			continue;
		}
		if (const auto index = channel_index(reading.attribute.id)) {
			result.channels[*index].valid = valid;
			result.channels[*index].rms_micro_units = valid
				? reading.value : 0;
			continue;
		}
		const auto descriptor = mnc::meter::describe(reading.attribute);
		double value = 0.0;
		const char *unit = "";
		switch (reading.unit) {
		case mnc::meter::MeterUnit::MilliHertz:
			value = static_cast<double>(reading.value) / 1e3;
			unit = "Hz";
			break;
		case mnc::meter::MeterUnit::MicroVolts:
			value = static_cast<double>(reading.value) / 1e6;
			unit = "V";
			break;
		case mnc::meter::MeterUnit::MicroAmperes:
			value = static_cast<double>(reading.value) / 1e6;
			unit = "A";
			break;
		case mnc::meter::MeterUnit::Picowatts:
			value = static_cast<double>(reading.value) / 1e12;
			unit = "W";
			break;
		case mnc::meter::MeterUnit::PicoVoltAmperes:
			value = static_cast<double>(reading.value) / 1e12;
			unit = "VA";
			break;
		case mnc::meter::MeterUnit::PowerFactorMillionths:
			value = static_cast<double>(reading.value) / 1e6;
			unit = "PF";
			break;
		case mnc::meter::MeterUnit::Picovars:
			value = static_cast<double>(reading.value) / 1e12;
			unit = "var";
			break;
		case mnc::meter::MeterUnit::Millidegrees:
			/* The PL publishes the 0..359.999-degree convention directly. */
			value = static_cast<double>(reading.value) / 1000.0;
			unit = "deg";
			break;
		case mnc::meter::MeterUnit::RatioMillionths:
			value = static_cast<double>(reading.value) / 10000.0;
			unit = "%";
			break;
		case mnc::meter::MeterUnit::MicroWattHours:
			value = static_cast<double>(reading.value);
			unit = "uWh";
			break;
		case mnc::meter::MeterUnit::MicroVarHours:
			value = static_cast<double>(reading.value);
			unit = "uvarh";
			break;
		case mnc::meter::MeterUnit::MicroVoltAmpereHours:
			value = static_cast<double>(reading.value);
			unit = "uVAh";
			break;
		case mnc::meter::MeterUnit::MicroWatts:
			value = static_cast<double>(reading.value);
			unit = "uW";
			break;
		case mnc::meter::MeterUnit::CrestTenThousandths:
			value = static_cast<double>(reading.value) / 10000.0;
			unit = "crest";
			break;
		case mnc::meter::MeterUnit::CategoricalCode:
			value = static_cast<double>(reading.value);
			unit = "code";
			break;
		}
		result.readings.push_back({std::string(descriptor.key),
					   unit, valid, value});
	}
	return result;
}

void print_snapshot(const MeterSnapshot &snapshot, std::ostream &output)
{
	output << "\033[2J\033[HMSAP1 meter results"
	       << "  sequence=" << snapshot.sequence << "  generation=0x"
	       << std::hex << snapshot.configuration_generation << std::dec
	       << "  block=" << snapshot.block_sample_count << " samples\n\n";
	for (const auto &channel : snapshot.channels) {
		output << "CH" << channel.channel << ' ' << std::setw(3)
		       << channel.name << "  RMS=";
		if (channel.valid)
			output << std::fixed << std::setprecision(3)
			       << static_cast<double>(channel.rms_micro_units) / 1000000.0
			       << (channel.quantity == "voltage" ? " V" : " A");
		else
			output << "invalid";
		output << "  mean=" << channel.mean_micro_units
		       << " micro-units  rms_count=" << channel.rms_count << '\n';
	}
	output << "\nFrequency=";
	if (snapshot.frequency.valid)
		output << std::fixed << std::setprecision(3)
		       << static_cast<double>(snapshot.frequency.millihz) / 1000.0
		       << " Hz (" << snapshot.frequency.cycles_used << " cycles)";
	else if (!snapshot.frequency.enabled)
		output << "disabled";
	else if (snapshot.frequency.out_of_range)
		output << "unavailable (out of range)";
	else if (snapshot.frequency.timed_out)
		output << "unavailable (no signal)";
	else
		output << "unavailable (measuring)";
	output << "\nPL capture=" << snapshot.capture_frames
	       << " header_errors=" << snapshot.header_errors
	       << " fifo_overflows=" << snapshot.fifo_overflows
	       << " emit_drops=" << snapshot.emit_drops
	       << " result_drops=" << snapshot.result_drops
	       << "\nCtrl-C to stop.\n" << std::flush;
}

int run_meter_view(const Options &options, std::ostream &output)
{
	AcquisitionClient client(options.socket_path);
	const auto started = Clock::now();
	std::optional<std::uint64_t> last_sequence;
	std::uint64_t displayed = 0;
	while (!stop_requested) {
		if (options.duration_seconds &&
		    std::chrono::duration<double>(Clock::now() - started).count() >=
			    *options.duration_seconds)
			break;
		if (options.result_limit && displayed >= *options.result_limit)
			break;
		const auto response = client.request(
			msap1::MeterSnapshotRequest{}, options.timeout_ms);
		require_daemon_ok(response.status);
		if (!response.running)
			throw std::runtime_error(
				"FPGA acquisition is stopped; run 'mnc adc start'");
		if (response.has_snapshot &&
		    (!last_sequence || response.snapshot.sequence != *last_sequence)) {
			const auto snapshot = meter_snapshot(response);
			print_snapshot(snapshot, output);
			last_sequence = snapshot.sequence;
			++displayed;
		}
		std::this_thread::sleep_for(50ms);
	}
	return 0;
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
		for (const auto &reading : snapshot.readings) {
			output << "  " << std::setw(20) << std::left
			       << reading.key << std::right << ' ';
			if (!reading.valid)
				output << "unavailable\n";
			else
				output << std::fixed << std::setprecision(
					       reading.unit == std::string("PF")
						       ? 4 : 3)
				       << reading.value << ' ' << reading.unit
				       << '\n';
		}
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

struct SingleCycleResult {
	bool running = false;
	bool has_snapshot = false;
	std::uint64_t records = 0;
	msap1::SingleCycleSnapshot snapshot{};
};

class SingleCycleTextGenerator final : public ResultGenerator<SingleCycleResult> {
public:
	int write(const SingleCycleResult &result,
		  std::ostream &output) const override
	{
		static constexpr std::array<const char *, 7> names{
			"Ia", "Ib", "Ic", "In", "Vc", "Vb", "Va"};
		static constexpr std::array<const char *, 3> pairs{
			"Vab", "Vbc", "Vca"};
		output << "Single-cycle diagnostic (SCYC-v5)\n"
		       << "  Records accepted:   " << result.records << '\n';
		if (!result.has_snapshot) {
			output << "  No snapshot yet (cycle timing unlocked or "
				  "acquisition stopped)\n";
			return 0;
		}
		const auto &s = result.snapshot;
		output << "  Sequence:           " << s.sequence << '\n'
		       << "  Cycle sequence:     " << s.cycle_sequence << '\n'
		       << "  Samples:            " << s.sample_count << '\n'
		       << "  Sample range:       " << s.first_sample << ".."
		       << s.last_sample << '\n'
		       << "  Processing tick:    " << s.processing_tick << '\n'
		       << "  Nominal / flags:    " << s.nominal_hz << " Hz / 0x"
		       << std::hex << s.flags << std::dec << '\n'
		       << "  Frequency:          " << s.frequency_millihz
		       << " mHz\n"
		       << "  Status:             0x" << std::hex << s.status
		       << std::dec << '\n';
		if (s.first_after_gap())
			output << "  Note: first whole cycle after a "
				  "discontinuity ("
			       << (s.gap_was_malformed() ? "malformed/dropped input"
			           : s.gap_was_timing() ? "cycle timing loss"
							: "reset or APPLY")
			       << ")\n";
		for (std::size_t lane = 0; lane < names.size(); ++lane)
			output << "  CH" << lane << ' ' << names[lane]
			       << " RMS: " << s.rms_micro_units[lane]
			       << " micro-units\n";
		for (std::size_t pair = 0; pair < pairs.size(); ++pair)
			output << "  " << pairs[pair]
			       << " RMS: " << s.vll_rms_micro_units[pair]
			       << " micro-units\n";
		static constexpr std::array<const char *, 3> phases{"A", "B",
								    "C"};
		for (std::size_t phase = 0; phase < phases.size(); ++phase)
			output << "  P" << phases[phase] << ": "
			       << s.active_power_picowatts[phase]
			       << " pW\n";
		if (!s.phasor_valid()) {
			output << "  Fundamental RMS:    invalid (no frequency "
				  "reference this cycle)\n";
			return 0;
		}
		for (std::size_t lane = 0; lane < names.size(); ++lane)
			output << "  CH" << lane << ' ' << names[lane]
			       << " fund RMS: "
			       << s.fundamental_rms_micro_units[lane]
			       << " micro-units\n";
		return 0;
	}
};

class SingleCycleJsonGenerator final : public ResultGenerator<SingleCycleResult> {
public:
	int write(const SingleCycleResult &result,
		  std::ostream &output) const override
	{
		write_json_success(output, result);
		return 0;
	}
};

int run_meter_single_cycle(const Options &options, std::ostream &output)
{
	AcquisitionClient client(options.socket_path);
	const auto response = client.request(
		msap1::SingleCycleRequest{}, options.timeout_ms);
	require_daemon_ok(response.status);
	SingleCycleResult result{response.running, response.has_snapshot,
				 response.records, response.snapshot};
	return render_result(options, result, output,
			     SingleCycleTextGenerator{},
			     SingleCycleJsonGenerator{});
}

struct PowerQualityResult {
	bool running = false;
	std::uint64_t records = 0;
	std::uint64_t events = 0;
	bool has_latest = false;
	bool has_event = false;
	msap1::PowerQualityIpcSnapshot latest{};
	msap1::PowerQualityIpcSnapshot event{};
};

const char *pq_event_type_name(std::uint8_t event_type)
{
	switch (event_type) {
	case 0: return "none";
	case 1: return "sag";
	case 2: return "swell";
	case 3: return "interruption";
	default: return "unknown";
	}
}

const char *pq_kind_name(std::uint8_t kind)
{
	switch (kind) {
	case 0: return "periodic";
	case 1: return "event start";
	case 2: return "event end";
	default: return "unknown";
	}
}

void write_power_quality_record(const msap1::PowerQualityIpcSnapshot &record,
				std::ostream &output)
{
	static constexpr std::array<const char *, 3> phases{"A", "B", "C"};
	output << "    Kind:            " << pq_kind_name(record.kind);
	if (record.kind != 0)
		output << " (" << pq_event_type_name(record.event_type) << ')';
	output << '\n'
	       << "    Sequence:        " << record.sequence << '\n'
	       << "    Span:            " << record.first_sample << ".."
	       << record.last_sample << " (" << record.sample_count
	       << " samples, " << record.half_cycle_updates << " updates)\n";
	if (record.kind != 0) {
		output << "    Affected phases: ";
		bool first = true;
		for (std::size_t phase = 0; phase < phases.size(); ++phase) {
			if ((record.affected_phases & (1u << phase)) == 0u)
				continue;
			output << (first ? "" : ", ") << phases[phase];
			first = false;
		}
		output << (first ? "none" : "") << '\n';
		output << "    Event sequence:  " << record.event_sequence
		       << '\n';
		if (record.duration_samples != 0 && record.sample_rate_hz != 0)
			output << "    Duration:        "
			       << record.duration_samples << " samples ("
			       << (static_cast<double>(record.duration_samples) *
				   1000.0 /
				   static_cast<double>(record.sample_rate_hz))
			       << " ms)\n";
	}
	for (std::size_t phase = 0; phase < phases.size(); ++phase) {
		const auto &lane = record.phases[phase];
		output << "    U" << phases[phase] << "rms(1/2): "
		       << static_cast<double>(lane.microvolts) / 1e6 << " V"
		       << "  [min " << static_cast<double>(lane.minimum_microvolts) / 1e6
		       << ", max " << static_cast<double>(lane.maximum_microvolts) / 1e6
		       << "]  I" << phases[phase] << "rms(1/2): "
		       << static_cast<double>(lane.microamperes) / 1e6 << " A\n";
	}
	output << "    Detection:       ";
	if (!record.armed) {
		output << "DISARMED (no reference configured)\n";
		return;
	}
	output << "reference "
	       << static_cast<double>(record.reference_microvolts) / 1e6
	       << " V, sag "
	       << static_cast<double>(record.sag_threshold_e4) / 100.0
	       << " %, swell "
	       << static_cast<double>(record.swell_threshold_e4) / 100.0
	       << " %, interruption "
	       << static_cast<double>(record.interruption_threshold_e4) / 100.0
	       << " %, hysteresis "
	       << static_cast<double>(record.hysteresis_e4) / 100.0 << " %\n";
}

class PowerQualityTextGenerator final
	: public ResultGenerator<PowerQualityResult> {
public:
	int write(const PowerQualityResult &result,
		  std::ostream &output) const override
	{
		output << "Power quality (PQEVT-v1, Urms(1/2))\n"
		       << "  Records accepted:   " << result.records << '\n'
		       << "  Events declared:    " << result.events << '\n';
		if (!result.has_latest) {
			output << "  No record yet (acquisition stopped or the "
				  "sliding tier is priming)\n";
			return 0;
		}
		output << "  Latest record:\n";
		write_power_quality_record(result.latest, output);
		if (!result.has_event) {
			output << "  No event edge seen since start\n";
			return 0;
		}
		output << "  Latest event edge:\n";
		write_power_quality_record(result.event, output);
		return 0;
	}
};

class PowerQualityJsonGenerator final
	: public ResultGenerator<PowerQualityResult> {
public:
	int write(const PowerQualityResult &result,
		  std::ostream &output) const override
	{
		write_json_success(output, result);
		return 0;
	}
};

int run_meter_power_quality(const Options &options, std::ostream &output)
{
	AcquisitionClient client(options.socket_path);
	const auto response = client.request(
		msap1::PowerQualityRequest{}, options.timeout_ms);
	require_daemon_ok(response.status);
	PowerQualityResult result{response.running, response.records,
				  response.events,  response.has_latest,
				  response.has_event, response.latest,
				  response.event};
	return render_result(options, result, output,
			     PowerQualityTextGenerator{},
			     PowerQualityJsonGenerator{});
}

const char *pq_lifecycle_name(msap1::PowerQualityEventLifecycle lifecycle)
{
	switch (lifecycle) {
	case msap1::PowerQualityEventLifecycle::start: return "start";
	case msap1::PowerQualityEventLifecycle::update: return "update";
	case msap1::PowerQualityEventLifecycle::end: return "end";
	case msap1::PowerQualityEventLifecycle::abort: return "abort";
	}
	return "unknown";
}

const char *pq_lifecycle_type_name(msap1::PowerQualityLifecycleType type)
{
	switch (type) {
	case msap1::PowerQualityLifecycleType::voltage_sag:
		return "voltage_sag";
	case msap1::PowerQualityLifecycleType::voltage_swell:
		return "voltage_swell";
	case msap1::PowerQualityLifecycleType::voltage_interruption:
		return "voltage_interruption";
	case msap1::PowerQualityLifecycleType::rapid_voltage_change:
		return "rapid_voltage_change";
	case msap1::PowerQualityLifecycleType::voltage_unbalance:
		return "voltage_unbalance";
	case msap1::PowerQualityLifecycleType::current_sag:
		return "current_sag";
	case msap1::PowerQualityLifecycleType::current_swell:
		return "current_swell";
	case msap1::PowerQualityLifecycleType::current_unbalance:
		return "current_unbalance";
	case msap1::PowerQualityLifecycleType::transient_voltage:
		return "transient_voltage";
	}
	return "unknown";
}

struct PowerQualityEventItem {
	std::string event_id;
	std::uint64_t source_session = 0;
	std::uint64_t source_counter = 0;
	std::string lifecycle;
	std::string type;
	bool iec_classification = false;
	std::uint8_t phase_mask = 0;
	std::uint32_t configuration_generation = 0;
	std::uint64_t first_sample = 0;
	std::uint64_t last_sample = 0;
	std::uint64_t trigger_sample = 0;
	std::uint64_t duration_samples = 0;
	double duration_ms = 0.0;
	std::optional<std::int64_t> start_utc_nanoseconds;
	std::optional<std::int64_t> last_utc_nanoseconds;
	std::optional<std::uint64_t> utc_uncertainty_nanoseconds;
	std::uint32_t status = 0;
	std::uint32_t discontinuities = 0;
	bool waveform_enabled = false;
	std::vector<std::string> waveform_capture_uuids;
};

struct PowerQualityEventsResult {
	std::uint32_t count = 0;
	std::uint32_t limit = 0;
	std::vector<std::string> export_formats{"mncwf"};
	std::vector<PowerQualityEventItem> events;
};

PowerQualityEventItem pq_event_item(
	const msap1::history::PowerQualityEventCatalogEntry &entry)
{
	const auto &event = entry.event;
	PowerQualityEventItem item{};
	item.event_id = msap1::mncwf_uuid_string(entry.event_uuid);
	item.source_session = event.id.session;
	item.source_counter = event.id.counter;
	item.lifecycle = pq_lifecycle_name(event.lifecycle);
	item.type = pq_lifecycle_type_name(event.type);
	item.iec_classification = event.iec_classification;
	item.phase_mask = event.phase_mask;
	item.configuration_generation = event.configuration_generation;
	item.first_sample = event.first_sample;
	item.last_sample = event.last_sample;
	item.trigger_sample = event.trigger_sample;
	item.duration_samples = event.duration_samples;
	item.duration_ms = event.sample_rate_hz == 0u ? 0.0
		: static_cast<double>(event.duration_samples) * 1000.0 /
			static_cast<double>(event.sample_rate_hz);
	item.start_utc_nanoseconds = entry.start_utc_nanoseconds;
	item.last_utc_nanoseconds = entry.last_utc_nanoseconds;
	item.utc_uncertainty_nanoseconds = entry.utc_uncertainty_nanoseconds;
	item.status = event.status;
	item.discontinuities = event.discontinuities;
	item.waveform_enabled = event.waveform_enabled;
	for (const auto &uuid : entry.waveform_capture_uuids)
		item.waveform_capture_uuids.push_back(
			msap1::mncwf_uuid_string(uuid));
	return item;
}

class PowerQualityEventsTextGenerator final
	: public ResultGenerator<PowerQualityEventsResult> {
public:
	int write(const PowerQualityEventsResult &result,
		std::ostream &output) const override
	{
		output << "Power-quality events: " << result.count << '\n';
		for (const auto &event : result.events) {
			output << "  " << event.event_id << "  " << event.type
			       << "  " << event.lifecycle << "  phases=0x"
			       << std::hex << static_cast<unsigned>(event.phase_mask)
			       << std::dec << "  duration=" << event.duration_ms
			       << " ms\n"
			       << "    samples " << event.first_sample << ".."
			       << event.last_sample << ", generation "
			       << event.configuration_generation << '\n';
			if (event.start_utc_nanoseconds)
				output << "    UTC " << *event.start_utc_nanoseconds
				       << ".."
				       << event.last_utc_nanoseconds.value_or(
						  *event.start_utc_nanoseconds)
				       << " ns\n";
			if (event.waveform_capture_uuids.empty())
				output << "    waveform: none\n";
			else
				for (const auto &capture : event.waveform_capture_uuids)
					output << "    waveform capture: " << capture
					       << '\n';
		}
		return 0;
	}
};

class PowerQualityEventsJsonGenerator final
	: public ResultGenerator<PowerQualityEventsResult> {
public:
	int write(const PowerQualityEventsResult &result,
		std::ostream &output) const override
	{
		write_json_success(output, result);
		return 0;
	}
};

int run_meter_power_quality_events(const Options &options,
	std::ostream &output)
{
	msap1::history::PowerQualityEventQuery query{};
	query.limit = static_cast<std::uint32_t>(
		options.result_limit.value_or(100u));
	if (query.limit == 0u || query.limit > 1000u)
		throw std::invalid_argument("--limit must be 1..1000");
	if (options.meter_event_id) {
		const auto uuid = msap1::mncwf_uuid_from_string(
			*options.meter_event_id);
		if (!uuid || msap1::mncwf_uuid_is_zero(*uuid))
			throw std::invalid_argument(
				"--event must be a nonzero canonical UUID");
		query.event_uuid = *uuid;
		query.limit = 1u;
	}
	query.start_utc_nanoseconds = options.meter_event_start_utc_ns;
	query.end_utc_nanoseconds = options.meter_event_end_utc_ns;
	if (query.start_utc_nanoseconds && query.end_utc_nanoseconds &&
	    *query.start_utc_nanoseconds > *query.end_utc_nanoseconds)
		throw std::invalid_argument("event UTC range is reversed");
	msap1::history::ipc::HistorianClient historian;
	const auto entries = historian.query_power_quality_events(query);
	PowerQualityEventsResult result{};
	result.limit = query.limit;
	result.count = static_cast<std::uint32_t>(entries.size());
	result.events.reserve(entries.size());
	for (const auto &entry : entries)
		result.events.push_back(pq_event_item(entry));
	return render_result(options, result, output,
		PowerQualityEventsTextGenerator{},
		PowerQualityEventsJsonGenerator{});
}

struct FlickerRecordResult {
	std::string kind;
	std::uint32_t sequence = 0;
	std::uint32_t configuration_generation = 0;
	std::uint32_t interval_seconds = 0;
	std::uint16_t lamp_voltage = 0;
	std::uint8_t nominal_frequency_hz = 0;
	std::uint8_t phase_valid_mask = 0;
	std::array<double, 3> pinst{};
	std::array<double, 3> pst{};
	std::array<double, 3> plt{};
	std::uint32_t status = 0;
};

struct FlickerResult {
	bool running = false;
	std::uint64_t records = 0;
	std::uint64_t sequence_gaps = 0;
	std::optional<FlickerRecordResult> live;
	std::optional<FlickerRecordResult> pst;
	std::optional<FlickerRecordResult> plt;
};

FlickerRecordResult flicker_result(const msap1::FlickerSnapshot &snapshot)
{
	FlickerRecordResult result{};
	result.kind = snapshot.kind == msap1::FlickerRecordKind::live ? "live"
		: snapshot.kind == msap1::FlickerRecordKind::pst ? "pst" : "plt";
	result.sequence = snapshot.sequence;
	result.configuration_generation = snapshot.configuration_generation;
	result.interval_seconds = snapshot.interval_seconds;
	result.lamp_voltage = snapshot.lamp_voltage;
	result.nominal_frequency_hz = snapshot.nominal_frequency_hz;
	result.phase_valid_mask = snapshot.phase_valid_mask;
	result.status = snapshot.status;
	for (std::size_t phase = 0; phase < 3u; ++phase) {
		result.pinst[phase] = snapshot.pinst_q16[phase] / 65536.0;
		result.pst[phase] = snapshot.pst_q16[phase] / 65536.0;
		result.plt[phase] = snapshot.plt_q16[phase] / 65536.0;
	}
	return result;
}

class FlickerTextGenerator final : public ResultGenerator<FlickerResult> {
public:
	int write(const FlickerResult &result,
		std::ostream &output) const override
	{
		static constexpr std::array<const char *, 3> phases{"A", "B", "C"};
		output << "Flicker records=" << result.records
		       << " gaps=" << result.sequence_gaps << '\n';
		const auto print = [&](const std::optional<FlickerRecordResult> &record) {
			if (!record)
				return;
			output << "  " << record->kind << " sequence=" << record->sequence
			       << " interval=" << record->interval_seconds << " s\n";
			for (std::size_t phase = 0; phase < 3u; ++phase)
				output << "    " << phases[phase]
				       << (record->phase_valid_mask & (1u << phase)
						? ": " : ": invalid ")
				       << "Pinst=" << record->pinst[phase]
				       << " Pst=" << record->pst[phase]
				       << " Plt=" << record->plt[phase] << '\n';
		};
		print(result.live); print(result.pst); print(result.plt);
		if (!result.live && !result.pst && !result.plt)
			output << "  no FLICKER-v1 record available\n";
		return 0;
	}
};

class FlickerJsonGenerator final : public ResultGenerator<FlickerResult> {
public:
	int write(const FlickerResult &result,
		std::ostream &output) const override
	{
		write_json_success(output, result);
		return 0;
	}
};

int run_meter_flicker(const Options &options, std::ostream &output)
{
	AcquisitionClient client(options.socket_path);
	const auto response = client.request(msap1::FlickerRequest{},
		options.timeout_ms);
	require_daemon_ok(response.status);
	FlickerResult result{response.running, response.records,
		response.sequence_gaps, {}, {}, {}};
	if (response.has_live) result.live = flicker_result(response.live);
	if (response.has_pst) result.pst = flicker_result(response.pst);
	if (response.has_plt) result.plt = flicker_result(response.plt);
	return render_result(options, result, output,
		FlickerTextGenerator{}, FlickerJsonGenerator{});
}

struct MainsSignalPhaseResult {
	std::string phase;
	bool valid = false;
	bool detected = false;
	double magnitude_volts = 0.0;
	double background_volts = 0.0;
};

struct MainsSignalResult {
	bool running = false;
	std::uint64_t records = 0;
	std::uint64_t sequence_gaps = 0;
	bool available = false;
	std::uint32_t sequence = 0;
	double configured_hz = 0.0;
	double measured_hz = 0.0;
	double bandwidth_hz = 0.0;
	std::uint32_t observation_ms = 0;
	double threshold_percent = 0.0;
	std::vector<MainsSignalPhaseResult> phases;
};

MainsSignalResult mains_signal_result(
	const msap1::MainsSignalResponse &response)
{
	static constexpr std::array<const char *, 3> phases{"A", "B", "C"};
	MainsSignalResult result{};
	result.running = response.running;
	result.records = response.records;
	result.sequence_gaps = response.sequence_gaps;
	result.available = response.has_snapshot;
	if (!response.has_snapshot)
		return result;
	const auto &snapshot = response.snapshot;
	result.sequence = snapshot.sequence;
	result.configured_hz = snapshot.configured_millihz / 1000.0;
	result.measured_hz = snapshot.measured_millihz / 1000.0;
	result.bandwidth_hz = snapshot.bandwidth_millihz / 1000.0;
	result.observation_ms = snapshot.observation_ms;
	result.threshold_percent = snapshot.threshold_e4 / 100.0;
	for (std::size_t phase = 0; phase < 3u; ++phase)
		result.phases.push_back({phases[phase],
			(snapshot.phase_valid_mask & (1u << phase)) != 0u,
			(snapshot.detected_phase_mask & (1u << phase)) != 0u,
			snapshot.magnitude_microvolts[phase] / 1e6,
			snapshot.background_microvolts[phase] / 1e6});
	return result;
}

class MainsSignalTextGenerator final
	: public ResultGenerator<MainsSignalResult> {
public:
	int write(const MainsSignalResult &result,
		std::ostream &output) const override
	{
		output << "Mains signalling records=" << result.records
		       << " gaps=" << result.sequence_gaps << '\n';
		if (!result.available) {
			output << "  no MAINS-SIGNAL-v1 observation available\n";
			return 0;
		}
		output << "  configured=" << result.configured_hz
		       << " Hz measured=" << result.measured_hz
		       << " Hz bandwidth=" << result.bandwidth_hz
		       << " Hz window=" << result.observation_ms << " ms\n";
		for (const auto &phase : result.phases)
			output << "    " << phase.phase << ": "
			       << (phase.valid ? "valid" : "invalid") << ' '
			       << (phase.detected ? "detected" : "not detected")
			       << " magnitude=" << phase.magnitude_volts
			       << " V background=" << phase.background_volts << " V\n";
		return 0;
	}
};

class MainsSignalJsonGenerator final
	: public ResultGenerator<MainsSignalResult> {
public:
	int write(const MainsSignalResult &result,
		std::ostream &output) const override
	{
		write_json_success(output, result);
		return 0;
	}
};

int run_meter_mains_signalling(const Options &options,
	std::ostream &output)
{
	AcquisitionClient client(options.socket_path);
	const auto response = client.request(msap1::MainsSignalRequest{},
		options.timeout_ms);
	require_daemon_ok(response.status);
	const auto result = mains_signal_result(response);
	return render_result(options, result, output,
		MainsSignalTextGenerator{}, MainsSignalJsonGenerator{});
}

struct HarmonicResult {
	bool running = false;
	std::uint64_t records = 0;
	std::uint64_t families = 0;
	std::uint64_t incomplete_families = 0;
	bool available = false;
	std::string period = "3s";
	msap1::HarmonicSpectrumSnapshot snapshot{};
};

msap1::MeasurementPeriod parse_harmonic_period(
	const std::optional<std::string> &value)
{
	if (!value || *value == "3s" || *value == "cycles_150_180")
		return msap1::MeasurementPeriod::Cycles150_180;
	if (*value == "10m" || *value == "minutes_10")
		return msap1::MeasurementPeriod::Min10;
	if (*value == "2h" || *value == "hours_2")
		return msap1::MeasurementPeriod::Hour2;
	if (*value == "base")
		return msap1::MeasurementPeriod::Basic;
	throw std::invalid_argument(
		"--period must be 3s, 10m, 2h, or base");
}

std::string harmonic_period_label(msap1::MeasurementPeriod period)
{
	switch (period) {
	case msap1::MeasurementPeriod::Cycles150_180: return "3s";
	case msap1::MeasurementPeriod::Min10: return "10m";
	case msap1::MeasurementPeriod::Hour2: return "2h";
	case msap1::MeasurementPeriod::Basic: return "base";
	default: throw std::invalid_argument("unsupported harmonic period");
	}
}

class HarmonicTextGenerator final : public ResultGenerator<HarmonicResult> {
public:
	int write(const HarmonicResult &result,
		  std::ostream &output) const override
	{
		output << "Harmonic spectrum (" << result.period
		       << ", IEC subgroups)\n"
		       << "  Chunks accepted:    " << result.records << '\n'
		       << "  Families completed: " << result.families << '\n'
		       << "  Families incomplete:" << ' '
		       << result.incomplete_families << '\n';
		if (!result.available) {
			output << "  No complete 42-record family yet\n";
			return 0;
		}
		const auto &spectrum = result.snapshot;
		output << "  Sequence:           " << spectrum.sequence << '\n'
		       << "  Window:             " << spectrum.cycle_count
		       << " cycles, " << spectrum.sample_count << " samples at "
		       << spectrum.sample_rate_hz << " Hz\n"
		       << "  Qualified orders:   1.."
		       << static_cast<unsigned>(spectrum.qualified_max_order)
		       << (spectrum.rate_limited() ? " (rate limited)" : "")
		       << '\n'
		       << "  Quality:            valid="
		       << yes_no(spectrum.interval_valid())
		       << ", arithmetic="
		       << (spectrum.arithmetic_error() ? "error" : "clean") << '\n';
		if (spectrum.aggregate_family())
			output << "  Contributors:       " << spectrum.contributors
			       << ", target sample " << spectrum.target_sample
			       << ", overshoot " << spectrum.overshoot_samples
			       << " sample(s)\n";
		else
			output << "  Measured frequency: "
			       << static_cast<double>(
				  spectrum.measured_frequency_millihz) / 1000.0
			       << " Hz\n";
		static constexpr std::array<const char *, 7> names{
			"Ia", "Ib", "Ic", "In", "Vc", "Vb", "Va"};
		for (std::size_t channel = 0; channel < spectrum.channels.size();
		     ++channel) {
			const char *unit = channel < 4 ? "A" : "V";
			output << "  " << names[channel] << " (order, magnitude "
			       << unit;
			if (!spectrum.aggregate_family())
				output << ", angle degrees relative to h*Va1";
			output << "):\n";
			for (const auto &point : spectrum.channels[channel]) {
				if (point.order > spectrum.qualified_max_order)
					break;
				output << "    " << std::setw(3)
				       << static_cast<unsigned>(point.order) << "  ";
				if (!point.magnitude_valid) {
					output << "unavailable\n";
					continue;
				}
				output << static_cast<double>(
						  point.magnitude_micro_units) /
						  1e6
				       << ' ' << unit << "  ";
				if (point.angle_valid)
					output << static_cast<double>(
						  point.angle_millidegrees) /
						  1000.0
					       << " deg\n";
				else
					output << "angle unavailable\n";
			}
		}
		return 0;
	}
};

class HarmonicJsonGenerator final : public ResultGenerator<HarmonicResult> {
public:
	int write(const HarmonicResult &result,
		  std::ostream &output) const override
	{
		write_json_success(output, result);
		return 0;
	}
};

int run_meter_harmonics(const Options &options, std::ostream &output)
{
	AcquisitionClient client(options.socket_path);
	msap1::HarmonicRequest request{};
	request.period = parse_harmonic_period(options.harmonic_period);
	const auto response = client.request(request, options.timeout_ms);
	require_daemon_ok(response.status);
	HarmonicResult result{response.running, response.records,
			      response.families, response.incomplete_families,
			      response.has_snapshot,
			      harmonic_period_label(response.period),
			      response.snapshot};
	return render_result(options, result, output, HarmonicTextGenerator{},
			     HarmonicJsonGenerator{});
}

struct ExactPhaseTotal {
	std::string phase_a;
	std::string phase_b;
	std::string phase_c;
	std::string total;
};

template<typename Unit>
ExactPhaseTotal exact(const msap1::PhaseABCTotal<msap1::Reading<Unit>> &group)
{
	return {std::to_string(group.phase_a.value),
		std::to_string(group.phase_b.value),
		std::to_string(group.phase_c.value),
		std::to_string(group.total.value)};
}

struct EnergyResult {
	ExactPhaseTotal active_import_uwh;
	ExactPhaseTotal active_export_uwh;
	ExactPhaseTotal apparent_uvah;
	std::array<ExactPhaseTotal, 4> reactive_quadrants_uvarh;
	std::string session_id;
	std::string last_sample_index;
	std::string accepted_samples;
	std::string skipped_samples;
	std::uint32_t accepted_blocks = 0;
	std::uint32_t skipped_blocks = 0;
	std::string reset_epoch;
	bool incomplete_accumulation = false;
	bool saturated = false;
	bool discontinuity = false;
};

EnergyResult energy_result(const msap1::EnergyValues &values)
{
	return {exact(values.active_import), exact(values.active_export),
		exact(values.apparent),
		{exact(values.reactive_quadrants[0]),
		 exact(values.reactive_quadrants[1]),
		 exact(values.reactive_quadrants[2]),
		 exact(values.reactive_quadrants[3])},
		std::to_string(values.session_id),
		std::to_string(values.last_sample_index),
		std::to_string(values.accepted_samples),
		std::to_string(values.skipped_samples), values.accepted_blocks,
		values.skipped_blocks, std::to_string(values.reset_epoch),
		values.incomplete_input, values.saturated, values.discontinuity};
}

void print_exact_group(std::ostream &output, std::string_view label,
	const ExactPhaseTotal &group, std::string_view unit)
{
	output << "  " << std::left << std::setw(30) << label << std::right
	       << " A=" << group.phase_a << " B=" << group.phase_b
	       << " C=" << group.phase_c << " total=" << group.total << ' '
	       << unit << '\n';
}

class EnergyTextGenerator final : public ResultGenerator<EnergyResult> {
public:
	int write(const EnergyResult &result, std::ostream &output) const override
	{
		output << "Four-quadrant lifetime energy\n";
		print_exact_group(output, "Active import", result.active_import_uwh,
			"uWh");
		print_exact_group(output, "Active export", result.active_export_uwh,
			"uWh");
		print_exact_group(output, "Apparent", result.apparent_uvah, "uVAh");
		static constexpr std::array labels{
			"Quadrant I  (P>=0, Q1>0)",
			"Quadrant II (P<0,  Q1>0)",
			"Quadrant III(P<0,  Q1<0)",
			"Quadrant IV (P>=0, Q1<0)"};
		for (std::size_t index = 0; index < labels.size(); ++index)
			print_exact_group(output, labels[index],
				result.reactive_quadrants_uvarh[index], "uvarh");
		output << "  Session ID: " << result.session_id
		       << "  reset epoch: " << result.reset_epoch
		       << "  last sample: " << result.last_sample_index << '\n'
		       << "  Accepted/skipped: " << result.accepted_samples << '/'
		       << result.skipped_samples << " samples, "
		       << result.accepted_blocks << '/' << result.skipped_blocks
		       << " blocks\n  Incomplete: "
		       << yes_no(result.incomplete_accumulation)
		       << "  saturated: " << yes_no(result.saturated)
		       << "  discontinuity: " << yes_no(result.discontinuity) << '\n';
		return 0;
	}
};

class EnergyJsonGenerator final : public ResultGenerator<EnergyResult> {
public:
	int write(const EnergyResult &result, std::ostream &output) const override
	{
		write_json_success(output, result);
		return 0;
	}
};

int run_meter_energy(const Options &options, std::ostream &output)
{
	msap1::meter_stream::MeterRecordStreamClient client;
	const auto values = client.energy();
	if (!values)
		throw std::runtime_error("no durable ENERGY checkpoint exists");
	const auto result = energy_result(*values);
	return render_result(options, result, output, EnergyTextGenerator{},
		EnergyJsonGenerator{});
}

struct DemandResult {
	ExactPhaseTotal current_active_uw;
	ExactPhaseTotal import_peak_uw;
	ExactPhaseTotal export_peak_uw;
	std::string session_id;
	std::string last_sample_index;
	std::string interval_anchor_sample;
	std::string peak_reset_epoch;
	std::string method;
	std::uint32_t window_seconds = 0;
	std::uint32_t update_seconds = 0;
	std::uint32_t profile_generation = 0;
	bool time_aligned = false;
	bool contaminated = false;
	bool boundary_valid = false;
	bool incomplete_accumulation = false;
	bool saturated = false;
};

DemandResult demand_result(const msap1::DemandValues &values)
{
	return {
		.current_active_uw = exact(values.current_active),
		.import_peak_uw = exact(values.import_peak),
		.export_peak_uw = exact(values.export_peak),
		.session_id = std::to_string(values.session_id),
		.last_sample_index = std::to_string(values.last_sample_index),
		.interval_anchor_sample = std::to_string(values.interval_anchor_sample),
		.peak_reset_epoch = std::to_string(values.peak_reset_epoch),
		.method = values.method == msap1::DemandMethod::fixed_block
			? "fixed_block" : "sliding",
		.window_seconds = values.window_seconds,
		.update_seconds = values.update_seconds,
		.profile_generation = values.profile_generation,
		.time_aligned = values.time_aligned,
		.contaminated = values.contaminated,
		.boundary_valid = values.boundary_valid,
		.incomplete_accumulation = values.incomplete_input,
		.saturated = values.saturated,
	};
}

class DemandTextGenerator final : public ResultGenerator<DemandResult> {
public:
	int write(const DemandResult &result, std::ostream &output) const override
	{
		output << "Active demand (" << result.method << ", "
		       << result.window_seconds << " s window, "
		       << result.update_seconds << " s update)\n";
		print_exact_group(output, "Current signed demand",
			result.current_active_uw, "uW");
		print_exact_group(output, "Session import peaks",
			result.import_peak_uw, "uW");
		print_exact_group(output, "Session export peaks",
			result.export_peak_uw, "uW");
		output << "  Session ID: " << result.session_id
		       << "  peak reset epoch: " << result.peak_reset_epoch
		       << "  interval anchor: " << result.interval_anchor_sample
		       << "  profile generation: " << result.profile_generation
		       << "\n  Aligned: " << yes_no(result.time_aligned)
		       << "  boundary valid: " << yes_no(result.boundary_valid)
		       << "  contaminated: " << yes_no(result.contaminated)
		       << "  incomplete: " << yes_no(result.incomplete_accumulation)
		       << "  saturated: " << yes_no(result.saturated) << '\n';
		return 0;
	}
};

class DemandJsonGenerator final : public ResultGenerator<DemandResult> {
public:
	int write(const DemandResult &result, std::ostream &output) const override
	{
		write_json_success(output, result);
		return 0;
	}
};

int run_meter_demand(const Options &options, std::ostream &output)
{
	msap1::meter_stream::MeterRecordStreamClient client;
	const auto values = client.demand();
	if (!values)
		throw std::runtime_error("no durable DEMAND checkpoint exists");
	const auto result = demand_result(*values);
	return render_result(options, result, output, DemandTextGenerator{},
		DemandJsonGenerator{});
}

std::string cli_actor()
{
	if (const auto *entry = ::getpwuid(::geteuid()); entry && entry->pw_name)
		return entry->pw_name;
	return "local-cli";
}

msap1::energy_ledger::ResetRequest reset_request(const Options &options)
{
	if (!options.meter_reset_confirm)
		throw std::invalid_argument("meter reset requires explicit --yes");
	if (!options.meter_reset_expected_epoch)
		throw std::invalid_argument("meter reset requires --expected-epoch");
	if (!options.meter_reset_idempotency_key ||
	    options.meter_reset_idempotency_key->empty())
		throw std::invalid_argument("meter reset requires --idempotency-key");
	const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
	return {*options.meter_reset_expected_epoch,
		*options.meter_reset_idempotency_key, cli_actor(),
		"mnc-" + std::to_string(now), now};
}

struct ResetResult {
	std::string reset_epoch;
	bool replayed = false;
};

int render_reset(const Options &options,
	const msap1::energy_ledger::ResetResult &reset, std::ostream &output)
{
	const ResetResult result{std::to_string(reset.epoch), reset.replayed};
	if (options.output_format == OutputFormat::json)
		write_json_success(output, result);
	else
		output << "Reset committed at epoch " << result.reset_epoch
		       << (result.replayed ? " (idempotent replay)" : "") << '\n';
	return 0;
}

int run_meter_energy_reset(const Options &options, std::ostream &output)
{
	msap1::meter_stream::MeterRecordStreamClient client;
	return render_reset(options, client.reset_energy(reset_request(options)),
		output);
}

int run_meter_demand_reset(const Options &options, std::ostream &output)
{
	msap1::meter_stream::MeterRecordStreamClient client;
	return render_reset(options,
		client.reset_demand_peaks(reset_request(options)), output);
}

std::uint64_t parse_reset_epoch(const std::string &value)
{
	std::size_t end = 0;
	std::uint64_t result = 0;
	try {
		result = std::stoull(value, &end, 10);
	} catch (const std::exception &) {
		throw std::invalid_argument(
			"--expected-epoch requires an unsigned decimal integer");
	}
	if (end != value.size())
		throw std::invalid_argument(
			"--expected-epoch requires an unsigned decimal integer");
	return result;
}

void add_reset_options(Command &command)
{
	command.add_option({
		"expected-epoch", "EPOCH", "Require the current reset epoch",
		CompletionKind::none,
		[](Options &options, const std::string &value) {
			options.meter_reset_expected_epoch = parse_reset_epoch(value);
		},
	});
	command.add_option({
		"idempotency-key", "KEY", "Stable retry key for this reset",
		CompletionKind::none,
		[](Options &options, const std::string &value) {
			options.meter_reset_idempotency_key = value;
		},
	});
	command.add_option({
		"yes", "", "Confirm resetting all values",
		CompletionKind::none,
		[](Options &options, const std::string &) {
			options.meter_reset_confirm = true;
		}, false,
	});
}

int run_meter_snapshot(const Options &options, std::ostream &output)
{
	AcquisitionClient client(options.socket_path);
	const auto response = client.request(
		msap1::MeterSnapshotRequest{}, options.timeout_ms);
	require_daemon_ok(response.status);
	const auto result = meter_snapshot(response);
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
	Command wiring("wiring", "Inspect or configure ADC current-channel wiring");
	wiring.add_subcommand(Command(
		"show", "Show requested and active ADC current wiring",
		run_meter_wiring_show,
		{
			.access = AccessLevel::diagnostic,
			.side_effect = SideEffect::none,
			.supports_text = true,
			.supports_json = true,
			.variants = {},
		}));
	Command wiring_set(
		"set", "Apply and persist ADC current-channel wiring",
		run_meter_wiring_set,
		{
			.access = AccessLevel::operator_control,
			.side_effect = SideEffect::control,
			.supports_text = true,
			.supports_json = true,
			.variants = {},
		});
	wiring_set.add_option({
		"preset", "abc|acb", "Set the phase-order preset (directions retained)",
		CompletionKind::none,
		[](Options &options, const std::string &value) {
			if (value != "abc" && value != "acb")
				throw std::invalid_argument("--preset must be abc or acb");
			options.current_wiring_preset = value;
		},
	});
	for (std::size_t channel = 0; channel < 4u; ++channel) {
		wiring_set.add_option(current_wiring_option(
			"ch" + std::to_string(channel) + "-phase",
			"Logical phase connected to ADC CH" + std::to_string(channel),
			channel, false));
		wiring_set.add_option(current_wiring_option(
			"ch" + std::to_string(channel) + "-direction",
			"Polarity of ADC CH" + std::to_string(channel),
			channel, true));
	}
	wiring.add_subcommand(std::move(wiring_set));
	meter.add_subcommand(std::move(wiring));
	meter.add_subcommand(Command(
		"single-cycle", "Show the latest single-cycle diagnostic",
		run_meter_single_cycle,
		{
			.access = AccessLevel::diagnostic,
			.side_effect = SideEffect::none,
			.supports_text = true,
			.supports_json = true,
			.variants = {},
		}));
	Command power_quality(
		"power-quality", "Show power-quality diagnostics and durable events",
		run_meter_power_quality,
		{
			.access = AccessLevel::diagnostic,
			.side_effect = SideEffect::none,
			.supports_text = true,
			.supports_json = true,
			.variants = {},
		});
	Command power_quality_events(
		"events", "List or inspect durable M18 power-quality events",
		run_meter_power_quality_events,
		{
			.access = AccessLevel::diagnostic,
			.side_effect = SideEffect::none,
			.supports_text = true,
			.supports_json = true,
			.variants = {},
		});
	power_quality_events.add_option({
		"event", "UUID", "Select one canonical event UUID",
		CompletionKind::none,
		[](Options &options, const std::string &value) {
			options.meter_event_id = value;
		},
	});
	power_quality_events.add_option({
		"start-utc-ns", "NS", "Include events ending at/after UTC",
		CompletionKind::none,
		[](Options &options, const std::string &value) {
			options.meter_event_start_utc_ns =
				parse_signed_integer(value, "--start-utc-ns");
		},
	});
	power_quality_events.add_option({
		"end-utc-ns", "NS", "Include events starting at/before UTC",
		CompletionKind::none,
		[](Options &options, const std::string &value) {
			options.meter_event_end_utc_ns =
				parse_signed_integer(value, "--end-utc-ns");
		},
	});
	power_quality_events.add_option({
		"limit", "COUNT", "Maximum events (default 100, maximum 1000)",
		CompletionKind::none,
		[](Options &options, const std::string &value) {
			options.result_limit = parse_positive_integer(value, "--limit");
		},
	});
	power_quality.add_subcommand(std::move(power_quality_events));
	meter.add_subcommand(std::move(power_quality));
	meter.add_subcommand(Command(
		"flicker", "Show the latest independent live, Pst, and Plt values",
		run_meter_flicker,
		{
			.access = AccessLevel::diagnostic,
			.side_effect = SideEffect::none,
			.supports_text = true,
			.supports_json = true,
			.variants = {},
		}));
	meter.add_subcommand(Command(
		"mains-signalling", "Show the latest mains-carrier observation",
		run_meter_mains_signalling,
		{
			.access = AccessLevel::diagnostic,
			.side_effect = SideEffect::none,
			.supports_text = true,
			.supports_json = true,
			.variants = {},
		}));
	Command harmonics(
		"harmonics", "Show the latest complete harmonic spectrum",
		run_meter_harmonics,
		{
			.access = AccessLevel::diagnostic,
			.side_effect = SideEffect::none,
			.supports_text = true,
			.supports_json = true,
			.variants = {},
		});
	harmonics.add_option({
		"period", "PERIOD", "Harmonic period: 3s, 10m, 2h, or base",
		CompletionKind::none,
		[](Options &options, const std::string &value) {
			options.harmonic_period = value;
		},
	});
	meter.add_subcommand(std::move(harmonics));
	Command energy(
		"energy", "Show durable four-quadrant lifetime energy",
		run_meter_energy,
		{
			.access = AccessLevel::diagnostic,
			.side_effect = SideEffect::none,
			.supports_text = true,
			.supports_json = true,
			.variants = {},
		});
	Command energy_reset(
		"reset", "Reset all authoritative lifetime energy counters",
		run_meter_energy_reset,
		{
			.access = AccessLevel::maintenance,
			.side_effect = SideEffect::control,
			.supports_text = true,
			.supports_json = true,
			.variants = {},
		});
	add_reset_options(energy_reset);
	energy.add_subcommand(std::move(energy_reset));
	meter.add_subcommand(std::move(energy));
	Command demand(
		"demand", "Show configured active demand and session peaks",
		run_meter_demand,
		{
			.access = AccessLevel::diagnostic,
			.side_effect = SideEffect::none,
			.supports_text = true,
			.supports_json = true,
			.variants = {},
		});
	Command demand_reset(
		"peaks-reset", "Reset all authoritative demand peaks",
		run_meter_demand_reset,
		{
			.access = AccessLevel::maintenance,
			.side_effect = SideEffect::control,
			.supports_text = true,
			.supports_json = true,
			.variants = {},
		});
	add_reset_options(demand_reset);
	demand.add_subcommand(std::move(demand_reset));
	meter.add_subcommand(std::move(demand));
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
