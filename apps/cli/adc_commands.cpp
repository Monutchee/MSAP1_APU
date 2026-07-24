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

double src_derived_rate(const msap1_adc_health_payload &health)
{
	if (health.dclk_frequency_hz == 0u)
		return 0.0;
	const auto src_n = static_cast<std::uint16_t>(
		(static_cast<std::uint16_t>(health.src_n_msb & 0x0fu) << 8) |
		health.src_n_lsb);
	const auto src_if = static_cast<std::uint16_t>(
		(static_cast<std::uint16_t>(health.src_if_msb) << 8) |
		health.src_if_lsb);
	const auto decimation = static_cast<double>(src_n) +
		static_cast<double>(src_if) / 65536.0;
	if (decimation <= 0.0)
		return 0.0;
	const auto dclk_divisor =
		static_cast<double>(1u << ((health.dout_format >> 1) & 0x07u));
	const auto modulator_divisor =
		(health.general_user_config_1 & 0x40u) != 0u ? 4.0 : 8.0;
	return static_cast<double>(health.dclk_frequency_hz) * dclk_divisor /
		(modulator_divisor * decimation);
}

int run_rate(const Options &options, std::ostream &output)
{
	AcquisitionClient client(options.socket_path);
	AcquisitionResponse response;
	if (options.sample_rate_hz) {
		response = client.request(AcquisitionCommand::sample_rate_set,
			options.timeout_ms, nullptr, *options.sample_rate_hz);
		require_daemon_ok(response);
		// The PL diagnostic meter publishes one-second snapshots. Waiting
		// beyond one complete window prevents this command from reporting
		// the measurement that preceded the rate transition.
		std::this_thread::sleep_for(std::chrono::milliseconds(1500));
		response = client.request(AcquisitionCommand::health,
			options.timeout_ms);
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
	application.add_command(std::move(adc));
}

} // namespace msap1::cli
