#include "cli.hpp"

#include "msap1/acquisition_ipc.hpp"
#include "msap1/meter_config.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace msap1::cli {
namespace {

void require_daemon_ok(const AcquisitionResponse &response)
{
	if (response.status != AcquisitionStatus::ok)
		throw std::runtime_error("acquisition daemon request failed (status " +
			std::to_string(static_cast<std::uint32_t>(response.status)) + ")");
}

int run_control(const Options &options, std::ostream &output,
		AcquisitionCommand command)
{
	AcquisitionClient client(options.socket_path);
	const auto response = client.request(command, options.timeout_ms);
	require_daemon_ok(response);
	output << "FPGA acquisition "
	       << (response.running != 0u ? "running" : "stopped") << '\n';
	return 0;
}

std::uint32_t parse_sample_rate(const std::string &value)
{
	std::size_t end = 0;
	std::uint64_t parsed = 0;
	try {
		parsed = std::stoull(value, &end, 0);
	} catch (const std::exception &) {
		throw std::invalid_argument("--sps requires a supported sample rate");
	}
	if (end != value.size() ||
	    parsed > static_cast<std::uint64_t>(
			     std::numeric_limits<std::uint32_t>::max()) ||
	    !supported_adc_sample_rate(static_cast<std::uint32_t>(parsed)))
		throw std::invalid_argument(
			"--sps must be one of 1000, 2000, 4000, 8000, 16000, "
			"32000, 64000, or 128000");
	return static_cast<std::uint32_t>(parsed);
}

double src_derived_rate(std::uint32_t dclk_frequency_hz,
			std::uint8_t general_user_config_1,
			std::uint8_t dout_format,
			std::uint8_t src_n_msb, std::uint8_t src_n_lsb,
			std::uint8_t src_if_msb, std::uint8_t src_if_lsb)
{
	if (dclk_frequency_hz == 0u)
		return 0.0;
	const auto src_n = static_cast<std::uint16_t>(
		(static_cast<std::uint16_t>(src_n_msb & 0x0fu) << 8) |
		src_n_lsb);
	const auto src_if = static_cast<std::uint16_t>(
		(static_cast<std::uint16_t>(src_if_msb) << 8) | src_if_lsb);
	const auto decimation = static_cast<double>(src_n) +
		static_cast<double>(src_if) / 65536.0;
	if (decimation <= 0.0)
		return 0.0;
	const auto dclk_divisor =
		static_cast<double>(1u << ((dout_format >> 1) & 0x07u));
	const auto modulator_divisor =
		(general_user_config_1 & 0x40u) != 0u ? 4.0 : 8.0;
	return static_cast<double>(dclk_frequency_hz) * dclk_divisor /
		(modulator_divisor * decimation);
}

double src_derived_rate(const msap1_adc_health_payload &health)
{
	return src_derived_rate(
		health.dclk_frequency_hz, health.general_user_config_1,
		health.dout_format, health.src_n_msb, health.src_n_lsb,
		health.src_if_msb, health.src_if_lsb);
}

bool rate_measurements_agree(std::uint32_t first, std::uint32_t second)
{
	if (first == 0u || second == 0u)
		return false;
	const auto difference = first > second ? first - second : second - first;
	return difference <= std::max(2u, second / 1000u);
}

int run_rate(const Options &options, std::ostream &output)
{
	AcquisitionClient client(options.socket_path);
	AcquisitionResponse response;
	if (options.sample_rate_hz) {
		response = client.request(AcquisitionCommand::sample_rate_set,
			options.timeout_ms, nullptr, *options.sample_rate_hz);
		require_daemon_ok(response);
		/*
		 * The PL publishes DRDY over independent one-second windows. Poll
		 * across window boundaries until two consecutive measurements agree;
		 * a single fixed delay can still return the pre-transition window.
		 */
		std::uint32_t previous_measurement = 0u;
		for (unsigned int attempt = 0; attempt < 5; ++attempt) {
			std::this_thread::sleep_for(
				std::chrono::milliseconds(1100));
			response = client.request(AcquisitionCommand::health,
				options.timeout_ms);
			require_daemon_ok(response);
			const auto current = response.rpu_health.drdy_frequency_hz;
			if (rate_measurements_agree(previous_measurement, current))
				break;
			previous_measurement = current;
		}
	} else {
		response = client.request(AcquisitionCommand::health,
			options.timeout_ms);
	}
	require_daemon_ok(response);

	const auto &health = response.rpu_health;
	const auto derived = src_derived_rate(health);
	const auto measured = health.drdy_frequency_hz;
	const auto requested = response.sample_rate_hz;
	const bool measurement_available = measured != 0u;
	const bool matches = measurement_available &&
		std::abs(static_cast<double>(measured) -
			 static_cast<double>(requested)) <=
			std::max(2.0, static_cast<double>(requested) * 0.01);

	output << "ADC sample rate\n"
	       << "  Requested:            " << requested << " frame/s\n"
	       << "  ADC DCLK:             ";
	if (health.dclk_frequency_hz != 0u)
		output << health.dclk_frequency_hz << " Hz\n";
	else
		output << "unavailable\n";
	output << "  SRC-derived rate:     ";
	if (derived > 0.0)
		output << std::fixed << std::setprecision(3) << derived
		       << " frame/s\n";
	else
		output << "unavailable\n";
	output << "  Measured ADC DRDY:    ";
	if (measurement_available)
		output << measured << " frame/s\n";
	else
		output << "unavailable\n";
	output << "  Result:               "
	       << (!measurement_available ? "measurement unavailable" :
		   matches ? "match" : "MISMATCH")
	       << '\n';
	return 0;
}

std::uint32_t parse_diagnostic_flow(const std::string &value)
{
	std::size_t end = 0;
	std::uint64_t parsed = 0;
	try {
		parsed = std::stoull(value, &end, 0);
	} catch (const std::exception &) {
		throw std::invalid_argument("--flow requires diagnostic flow 1");
	}
	if (end != value.size() || parsed != 1u)
		throw std::invalid_argument("--flow currently supports only flow 1");
	return 1u;
}

const char *diagnostic_stage_name(std::uint32_t stage)
{
	switch (stage) {
	case MSAP1_ADC_DIAGNOSTIC_STAGE_NONE: return "none";
	case MSAP1_ADC_DIAGNOSTIC_STAGE_PREFLIGHT: return "preflight";
	case MSAP1_ADC_DIAGNOSTIC_STAGE_BEFORE: return "before snapshot";
	case MSAP1_ADC_DIAGNOSTIC_STAGE_RESET_ASSERT: return "RESET_N asserted";
	case MSAP1_ADC_DIAGNOSTIC_STAGE_RESET_RELEASE: return "RESET_N release";
	case MSAP1_ADC_DIAGNOSTIC_STAGE_RESET_DEFAULTS:
		return "reset-default snapshot";
	case MSAP1_ADC_DIAGNOSTIC_STAGE_RECONFIGURE:
		return "conservative SRC reload";
	case MSAP1_ADC_DIAGNOSTIC_STAGE_AFTER: return "after snapshot";
	default: return "unknown";
	}
}

const char *diagnostic_error_name(std::uint32_t error)
{
	switch (error) {
	case MSAP1_ADC_DIAGNOSTIC_ERROR_NONE: return "none";
	case MSAP1_ADC_DIAGNOSTIC_ERROR_NOT_INITIALIZED:
		return "ADC not initialized";
	case MSAP1_ADC_DIAGNOSTIC_ERROR_CAPTURE_ACTIVE:
		return "capture was not stopped";
	case MSAP1_ADC_DIAGNOSTIC_ERROR_SPI: return "SPI communication";
	case MSAP1_ADC_DIAGNOSTIC_ERROR_ADC_NOT_READY:
		return "INIT_COMPLETE timeout";
	case MSAP1_ADC_DIAGNOSTIC_ERROR_REGISTER_MISMATCH:
		return "register readback mismatch";
	default: return "internal error";
	}
}

void print_hex_byte(std::ostream &output, std::uint8_t value)
{
	output << "0x" << std::hex << std::setw(2) << std::setfill('0')
	       << static_cast<unsigned int>(value) << std::dec
	       << std::setfill(' ');
}

void print_diagnostic_snapshot(
	std::ostream &output, const char *name,
	const msap1_adc_diagnostic_snapshot &snapshot)
{
	output << "\n" << name << "\n"
	       << "  PL capture flags:     0x" << std::hex
	       << snapshot.capture_flags << std::dec << "\n"
	       << "  PL frames/packets:    " << snapshot.frame_count << " / "
	       << snapshot.packet_count << "\n"
	       << "  ADC DCLK:             ";
	if (snapshot.dclk_frequency_hz != 0u)
		output << snapshot.dclk_frequency_hz << " Hz\n";
	else
		output << "unavailable/0\n";
	output << "  ADC DRDY:             ";
	if (snapshot.drdy_frequency_hz != 0u)
		output << snapshot.drdy_frequency_hz << " frame/s\n";
	else
		output << "unavailable/0\n";

	if ((snapshot.snapshot_flags &
	     MSAP1_ADC_DIAGNOSTIC_SNAPSHOT_SPI_VALID) == 0u) {
		output << "  SPI registers:        unavailable while RESET_N is low\n";
		return;
	}

	const auto src_n = static_cast<std::uint16_t>(
		(static_cast<std::uint16_t>(snapshot.src_n_msb & 0x0fu) << 8) |
		snapshot.src_n_lsb);
	const auto src_if = static_cast<std::uint16_t>(
		(static_cast<std::uint16_t>(snapshot.src_if_msb) << 8) |
		snapshot.src_if_lsb);
	const auto derived = src_derived_rate(
		snapshot.dclk_frequency_hz,
		snapshot.general_user_config_1, snapshot.dout_format,
		snapshot.src_n_msb, snapshot.src_n_lsb,
		snapshot.src_if_msb, snapshot.src_if_lsb);

	output << "  STATUS 1/2/3:        ";
	print_hex_byte(output, snapshot.status_1);
	output << " / ";
	print_hex_byte(output, snapshot.status_2);
	output << " / ";
	print_hex_byte(output, snapshot.status_3);
	output << "\n  CONFIG 1/2/3:        ";
	print_hex_byte(output, snapshot.general_user_config_1);
	output << " / ";
	print_hex_byte(output, snapshot.general_user_config_2);
	output << " / ";
	print_hex_byte(output, snapshot.general_user_config_3);
	output << "\n  DOUT_FORMAT:          ";
	print_hex_byte(output, snapshot.dout_format);
	output << "\n  CHANNEL_DISABLE:      ";
	print_hex_byte(output, snapshot.channel_disable);
	output << "\n  BUFFER_CONFIG 1/2:   ";
	print_hex_byte(output, snapshot.buffer_config_1);
	output << " / ";
	print_hex_byte(output, snapshot.buffer_config_2);
	output << "\n  SRC N / IF:           " << src_n << " / " << src_if
	       << "\n  SRC_UPDATE:           ";
	print_hex_byte(output, snapshot.src_update);
	output << "\n  SRC-derived rate:     ";
	if (derived > 0.0)
		output << std::fixed << std::setprecision(3) << derived
		       << " frame/s\n";
	else
		output << "unavailable\n";
}

const char *yes_no(bool value)
{
	return value ? "yes" : "no";
}

int run_test_flow(const Options &options, std::ostream &output)
{
	if (!options.diagnostic_flow)
		throw std::invalid_argument("mnc adc testflw requires --flow 1");

	AcquisitionClient client(options.socket_path);
	const auto timeout = std::max(options.timeout_ms, 20000);
	const auto response = client.request(
		AcquisitionCommand::adc_diagnostic_run, timeout, nullptr, 0u,
		*options.diagnostic_flow);
	require_daemon_ok(response);
	const auto &diagnostic = response.adc_diagnostic;
	const auto flags = diagnostic.diagnostic_flags;

	output << "AD7771 diagnostic test flow " << diagnostic.flow << "\n"
	       << "  Reset method:         PL-driven ADC RESET_N pulse\n"
	       << "  FPGA/Linux reset:     no\n"
	       << "  ADC power cycle:      no\n"
	       << "  RESET_N hold:         " << diagnostic.reset_hold_ms << " ms\n"
	       << "  Requested sample rate:" << std::setw(9)
	       << diagnostic.requested_sample_rate_hz << " frame/s\n"
	       << "  Flow error:           "
	       << diagnostic_error_name(diagnostic.diagnostic_error) << "\n"
	       << "  Failure stage:        "
	       << diagnostic_stage_name(diagnostic.failure_stage) << "\n"
	       << "  Acquisition restored: "
	       << yes_no(response.running != 0u) << "\n";

	print_diagnostic_snapshot(output, "Before reset", diagnostic.before);
	print_diagnostic_snapshot(
		output, "While RESET_N asserted", diagnostic.reset_asserted);
	print_diagnostic_snapshot(
		output, "Reset defaults (before configuration)",
		diagnostic.reset_defaults);
	print_diagnostic_snapshot(
		output, "After conservative SRC reload", diagnostic.after);

	output << "\nFlow checks\n"
	       << "  RESET_N commanded:    "
	       << yes_no((flags & MSAP1_ADC_DIAGNOSTIC_RESET_ASSERTED) != 0u)
	       << "\n  DRDY stopped in reset:"
	       << std::setw(9)
	       << yes_no((flags &
			  MSAP1_ADC_DIAGNOSTIC_RESET_DRDY_STOPPED) != 0u)
	       << "\n  Reset defaults read:  "
	       << yes_no((flags &
			  MSAP1_ADC_DIAGNOSTIC_RESET_DEFAULTS_READ) != 0u)
	       << "\n  SRC_UPDATE read high: "
	       << yes_no((flags &
			  MSAP1_ADC_DIAGNOSTIC_SRC_UPDATE_HIGH_READ) != 0u)
	       << " (";
	print_hex_byte(output, diagnostic.src_update_high_readback);
	output << ")\n  SRC_UPDATE read low:  "
	       << yes_no((flags &
			  MSAP1_ADC_DIAGNOSTIC_SRC_UPDATE_LOW_READ) != 0u)
	       << " (";
	print_hex_byte(output, diagnostic.src_update_low_readback);
	output << ")\n  SRC holding match:    "
	       << yes_no((flags &
			  MSAP1_ADC_DIAGNOSTIC_SRC_HOLDING_MATCH) != 0u)
	       << "\n  Final config match:   "
	       << yes_no((flags &
			  MSAP1_ADC_DIAGNOSTIC_FINAL_CONFIG_MATCH) != 0u)
	       << "\n  Final DRDY match:     "
	       << yes_no((flags &
			  MSAP1_ADC_DIAGNOSTIC_FINAL_DRDY_MATCH) != 0u)
	       << "\n  Restored live DRDY:   ";
	if (response.rpu_health.drdy_frequency_hz != 0u)
		output << response.rpu_health.drdy_frequency_hz << " frame/s\n";
	else
		output << "unavailable\n";

	if (diagnostic.diagnostic_error !=
	    MSAP1_ADC_DIAGNOSTIC_ERROR_NONE)
		output << "\nConclusion: Flow did not complete; use the failure stage "
			  "and snapshots above.\n";
	else if ((flags & MSAP1_ADC_DIAGNOSTIC_FINAL_DRDY_MATCH) == 0u)
		output << "\nConclusion: RESET_N and the conservative SRC load "
			  "completed, but physical DRDY still does not match the "
			  "requested rate.\n";
	else
		output << "\nConclusion: Physical DRDY matches the requested rate "
			  "after reset and SRC reload.\n";
	return 0;
}

} // namespace

void register_adc_commands(Application &application)
{
	Command adc("adc", "Control ADC capture and FPGA acquisition");
	adc.add_subcommand(Command("start", "Start ADC capture and meter acquisition",
		[](const Options &options, std::ostream &output) {
			return run_control(options, output, AcquisitionCommand::start);
		}));
	adc.add_subcommand(Command("stop", "Stop ADC capture and meter acquisition",
		[](const Options &options, std::ostream &output) {
			return run_control(options, output, AcquisitionCommand::stop);
		}));
	Command rate("rate", "Inspect or temporarily set the ADC sample rate",
		     run_rate);
	rate.add_option({
		"sps", "RATE", "Temporary sample rate in frames per second",
		CompletionKind::none,
		[](Options &options, const std::string &value) {
			options.sample_rate_hz = parse_sample_rate(value);
		},
	});
	adc.add_subcommand(std::move(rate));
	Command test_flow(
		"testflw",
		"Run a destructive, self-restoring ADC diagnostic flow",
		run_test_flow);
	test_flow.add_option({
		"flow", "NUMBER", "Diagnostic flow (currently: 1)",
		CompletionKind::none,
		[](Options &options, const std::string &value) {
			options.diagnostic_flow = parse_diagnostic_flow(value);
		},
	});
	adc.add_subcommand(std::move(test_flow));
	application.add_command(std::move(adc));
}

} // namespace msap1::cli
