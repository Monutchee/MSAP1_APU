#include "cli.hpp"

#include "msap1/acquisition_ipc.hpp"
#include "msap1/meter_health.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

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

void require_daemon_ok(const AcquisitionResponse &response)
{
	if (response.status != AcquisitionStatus::ok)
		throw std::runtime_error("acquisition daemon request failed (status " +
			std::to_string(static_cast<std::uint32_t>(response.status)) + ")");
}

int run_meter_health(const Options &options, std::ostream &output)
{
	AcquisitionClient client(options.socket_path);
	const auto response =
		client.request(AcquisitionCommand::health, options.timeout_ms);
	require_daemon_ok(response);
	const auto status = evaluate_meter_health(response);
	const auto &health = response.rpu_health;

	output << "MSAP1 meter health: " << (status.healthy ? "PASS" : "FAIL") << '\n'
	       << "  Linux acquisition:    " << yes_no(response.running != 0u) << '\n'
	       << "  Meter record present: " << yes_no(response.has_meter_record != 0u)
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
	       << "  Capture active:       " << yes_no(status.capture_active) << '\n'
	       << "  Sample rate:          " << health.sample_rate_hz << " frame/s\n"
	       << "  PL frames:            " << health.frame_count << '\n'
	       << "  FIFO overflows:       " << health.overflow_count << '\n'
	       << "  Header errors:        " << health.header_error_count << '\n'
	       << "  Conversion status:   0x" << std::hex << health.conversion_status
	       << '\n'
	       << "  Processing status:   0x" << health.processing_status << std::dec
	       << '\n';
	return status.healthy ? 0 : 1;
}

void print_record(const MeterRecord &record, std::ostream &output)
{
	static constexpr std::array<const char *, 8> names{
		"ILA", "ILB", "ILC", "ILN", "VLC", "VLB", "VLA", "VCM"};
	output << "\033[2J\033[HMSAP1 meter results"
	       << "  sequence=" << record.sequence() << "  generation=0x" << std::hex
	       << record.configuration_generation() << std::dec
	       << "  window=" << record.window_samples() << " samples\n\n";
	for (std::size_t index = 0; index < names.size(); ++index) {
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

} // namespace

void register_meter_commands(Application &application)
{
	Command meter("meter", "Inspect MSAP1 meter health and readings");
	meter.add_subcommand(Command("health", "Show acquisition and meter health",
				     run_meter_health));
	Command view("view", "Continuously display the latest meter readings",
		     run_meter_view);
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

} // namespace msap1::cli
