#include "msap1/acquisition_ipc.hpp"

#include <array>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

volatile std::sig_atomic_t stop_requested = 0;

enum class Command { meter_view, meter_health, adc_start, adc_stop };

struct Options {
	Command command = Command::meter_view;
	std::string socket_path = msap1::acquisition_socket_path;
	std::optional<std::uint64_t> result_limit;
	std::optional<double> duration_seconds;
	int timeout_ms = 3000;
};

void handle_signal(int) { stop_requested = 1; }

void usage(const char *program)
{
	std::cerr
		<< "Usage:\n"
		<< "  " << program << " meter-view [--results COUNT] [--duration SECONDS]\n"
		<< "  " << program << " meter-health\n"
		<< "  " << program << " adc-start\n"
		<< "  " << program << " adc-stop\n\n"
		<< "Common options:\n"
		<< "  --socket PATH       Acquisition daemon control socket\n"
		<< "  --timeout-ms MS     Daemon timeout (default: 3000)\n";
}

std::uint64_t parse_unsigned(const std::string &value,
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
	if (end != value.size() || result <= 0.0)
		throw std::invalid_argument("--duration requires a positive number");
	return result;
}

Options parse_options(int argc, char **argv)
{
	if (argc < 2)
		throw std::invalid_argument("expected meter-view, meter-health, adc-start, or adc-stop");
	Options options;
	const std::string command = argv[1];
	if (command == "meter-view")
		options.command = Command::meter_view;
	else if (command == "meter-health")
		options.command = Command::meter_health;
	else if (command == "adc-start")
		options.command = Command::adc_start;
	else if (command == "adc-stop")
		options.command = Command::adc_stop;
	else if (command == "--help" || command == "-h") {
		usage(argv[0]);
		std::exit(0);
	} else {
		throw std::invalid_argument("unknown command '" + command + "'");
	}

	for (int index = 2; index < argc; ++index) {
		const std::string option = argv[index];
		if (option == "--help" || option == "-h") {
			usage(argv[0]);
			std::exit(0);
		}
		if (index + 1 >= argc)
			throw std::invalid_argument(option + " requires a value");
		const std::string value = argv[++index];
		if (option == "--results") {
			if (options.command != Command::meter_view)
				throw std::invalid_argument("--results is only valid for meter-view");
			options.result_limit = parse_unsigned(value, option);
		} else if (option == "--duration") {
			if (options.command != Command::meter_view)
				throw std::invalid_argument("--duration is only valid for meter-view");
			options.duration_seconds = parse_duration(value);
		} else if (option == "--socket") {
			options.socket_path = value;
		} else if (option == "--timeout-ms") {
			const auto timeout = parse_unsigned(value, option);
			if (timeout > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
				throw std::invalid_argument("--timeout-ms is too large");
			options.timeout_ms = static_cast<int>(timeout);
		} else {
			throw std::invalid_argument("unknown option '" + option + "'");
		}
	}
	return options;
}

bool health_flag(const msap1_adc_health_payload &health, std::uint32_t flag)
{
	return (health.health_flags & flag) != 0u;
}

bool meter_flag(const msap1_adc_health_payload &health, std::uint32_t flag)
{
	return (health.meter_health_flags & flag) != 0u;
}

const char *yes_no(bool value) { return value ? "yes" : "no"; }

void require_daemon_ok(const msap1::AcquisitionResponse &response)
{
	if (response.status != msap1::AcquisitionStatus::ok)
		throw std::runtime_error("acquisition daemon request failed (status " +
			std::to_string(static_cast<std::uint32_t>(response.status)) + ")");
}

int run_meter_health(const Options &options)
{
	msap1::AcquisitionClient client(options.socket_path);
	const auto response = client.request(msap1::AcquisitionCommand::health,
					     options.timeout_ms);
	require_daemon_ok(response);
	const auto &health = response.rpu_health;
	const bool adc_ok = health_flag(health, MSAP1_ADC_HEALTH_SPI_RESPONSIVE) &&
		health_flag(health, MSAP1_ADC_HEALTH_INITIALIZED) &&
		health_flag(health, MSAP1_ADC_HEALTH_INIT_COMPLETE) &&
		health_flag(health, MSAP1_ADC_HEALTH_CONFIG_MATCH) &&
		health_flag(health, MSAP1_ADC_HEALTH_CAPTURE_ACTIVE) &&
		health_flag(health, MSAP1_ADC_HEALTH_NO_OVERFLOW) &&
		health_flag(health, MSAP1_ADC_HEALTH_HEADERS_VALID);
	const bool meter_ok = meter_flag(health, MSAP1_METER_HEALTH_CORES_PRESENT) &&
		meter_flag(health, MSAP1_METER_HEALTH_CONFIGURED) &&
		meter_flag(health, MSAP1_METER_HEALTH_GENERATION_MATCH) &&
		meter_flag(health, MSAP1_METER_HEALTH_ENABLED) &&
		meter_flag(health, MSAP1_METER_HEALTH_REMOVE_DC);
	const bool linux_ok = response.running != 0u &&
		response.has_meter_record != 0u && response.dma_read_errors == 0u &&
		response.invalid_records == 0u && response.sequence_gaps == 0u;
	const bool healthy = adc_ok && meter_ok && linux_ok &&
		health.meter_generation == response.configuration_generation;

	std::cout << "MSAP1 meter health: " << (healthy ? "PASS" : "FAIL") << '\n'
		<< "  Linux acquisition:    " << yes_no(response.running != 0u) << '\n'
		<< "  Meter record present: " << yes_no(response.has_meter_record != 0u) << '\n'
		<< "  Meter records:        " << response.meter_records << '\n'
		<< "  DMA bytes:            " << response.dma_bytes << '\n'
		<< "  DMA read errors:      " << response.dma_read_errors << '\n'
		<< "  Invalid records:      " << response.invalid_records << '\n'
		<< "  Sequence gaps:        " << response.sequence_gaps << '\n'
		<< "  Configuration gen:    0x" << std::hex
		<< response.configuration_generation << std::dec << '\n'
		<< "  PL generation match:  "
		<< yes_no(health.meter_generation == response.configuration_generation) << '\n'
		<< "  Meter configured:     "
		<< yes_no(meter_flag(health, MSAP1_METER_HEALTH_CONFIGURED)) << '\n'
		<< "  DC offset removal:    "
		<< yes_no(meter_flag(health, MSAP1_METER_HEALTH_REMOVE_DC)) << '\n'
		<< "  ADC SPI responsive:   "
		<< yes_no(health_flag(health, MSAP1_ADC_HEALTH_SPI_RESPONSIVE)) << '\n'
		<< "  Capture active:       "
		<< yes_no(health_flag(health, MSAP1_ADC_HEALTH_CAPTURE_ACTIVE)) << '\n'
		<< "  Sample rate:          " << health.sample_rate_hz << " frame/s\n"
		<< "  PL frames:            " << health.frame_count << '\n'
		<< "  FIFO overflows:       " << health.overflow_count << '\n'
		<< "  Header errors:        " << health.header_error_count << '\n'
		<< "  Conversion status:   0x" << std::hex << health.conversion_status << '\n'
		<< "  Processing status:   0x" << health.processing_status << std::dec << '\n';
	return healthy ? 0 : 1;
}

void print_record(const msap1::MeterRecord &record)
{
	static constexpr std::array<const char *, 8> names{
		"ILA", "ILB", "ILC", "ILN", "VLC", "VLB", "VLA", "VCM"};
	std::cout << "\033[2J\033[HMSAP1 meter results"
		<< "  sequence=" << record.sequence()
		<< "  generation=0x" << std::hex << record.configuration_generation()
		<< std::dec << "  window=" << record.window_samples() << " samples\n\n";
	for (std::size_t index = 0; index < names.size(); ++index) {
		const auto channel = record.channel(index);
		std::cout << "CH" << index << ' ' << std::setw(3) << names[index]
			<< "  RMS=";
		if (channel.valid)
			std::cout << std::fixed << std::setprecision(3)
				<< static_cast<double>(channel.rms_micro_units) / 1000000.0
				<< (index >= 4 && index <= 6 ? " V" : " A");
		else
			std::cout << "invalid";
		std::cout << "  mean=" << channel.mean_micro_units
			<< " micro-units  rms_count=" << channel.rms_count << '\n';
	}
	std::cout << "\nPL capture=" << record.capture_frames()
		<< " header_errors=" << record.header_errors()
		<< " fifo_overflows=" << record.fifo_overflows()
		<< " packetizer_drops=" << record.packetizer_drops()
		<< " hub_drops=" << record.hub_drops() << "\nCtrl-C to stop.\n"
		<< std::flush;
}

int run_meter_view(const Options &options)
{
	msap1::AcquisitionClient client(options.socket_path);
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
		const auto response = client.request(msap1::AcquisitionCommand::info,
						     options.timeout_ms);
		require_daemon_ok(response);
		if (response.running == 0u)
			throw std::runtime_error("FPGA acquisition is stopped; run adc-start");
		if (response.has_meter_record != 0u &&
		    (!last_sequence || response.latest_record.sequence() != *last_sequence)) {
			if (!response.latest_record.header_valid())
				throw std::runtime_error("daemon returned an invalid meter record");
			print_record(response.latest_record);
			last_sequence = response.latest_record.sequence();
			++displayed;
		}
		std::this_thread::sleep_for(50ms);
	}
	return 0;
}

int run_control(const Options &options, msap1::AcquisitionCommand command)
{
	msap1::AcquisitionClient client(options.socket_path);
	const auto response = client.request(command, options.timeout_ms);
	require_daemon_ok(response);
	std::cout << "FPGA acquisition "
		<< (response.running != 0u ? "running" : "stopped") << '\n';
	return 0;
}

int run(int argc, char **argv)
{
	const auto options = parse_options(argc, argv);
	switch (options.command) {
	case Command::meter_view: return run_meter_view(options);
	case Command::meter_health: return run_meter_health(options);
	case Command::adc_start:
		return run_control(options, msap1::AcquisitionCommand::start);
	case Command::adc_stop:
		return run_control(options, msap1::AcquisitionCommand::stop);
	}
	return 1;
}

} // namespace

int main(int argc, char **argv)
{
	std::signal(SIGINT, handle_signal);
	std::signal(SIGTERM, handle_signal);
	try {
		return run(argc, argv);
	} catch (const std::invalid_argument &error) {
		std::cerr << "msap1-apu-app: " << error.what() << "\n\n";
		usage(argv[0]);
		return 2;
	} catch (const std::exception &error) {
		std::cerr << "msap1-apu-app: " << error.what() << '\n';
		return 1;
	}
}
