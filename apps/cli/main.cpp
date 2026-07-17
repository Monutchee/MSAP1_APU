#include "msap1/acquisition_ipc.hpp"
#include "msap1/shared_ring.hpp"
#include "msap1/visualizer.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

volatile std::sig_atomic_t stop_requested = 0;

enum class Command {
	adc_view,
	adc_health,
	adc_start,
	adc_stop,
};

void handle_signal(int)
{
	stop_requested = 1;
}

struct Options {
	Command command = Command::adc_view;
	std::string socket_path = msap1::acquisition_socket_path;
	std::string shm_name = msap1::acquisition_shm_name;
	std::string format = "terminal";
	std::string output;
	std::uint32_t display_rate_hz = 1000;
	std::vector<std::size_t> channels;
	std::optional<std::uint64_t> frame_limit;
	std::optional<double> duration_seconds;
	int timeout_ms = 3000;
};

void usage(const char *program)
{
	std::cerr
		<< "Usage:\n"
		<< "  " << program << " adc-view [options]\n"
		<< "  " << program << " adc-health [options]\n"
		<< "  " << program << " adc-start [options]\n"
		<< "  " << program << " adc-stop [options]\n\n"
		<< "adc-view options:\n"
		<< "  --rate HZ           Shared-ring display rate (default: 1000)\n"
		<< "  --channels LIST     ADC channels to show, e.g. 4,5,6\n"
		<< "  --format FORMAT     terminal, table, csv, or jsonl\n"
		<< "  --output FILE       Write visualization/export to FILE\n"
		<< "  --frames COUNT      Stop after COUNT displayed frames\n"
		<< "  --duration SECONDS  Stop after elapsed SECONDS\n\n"
		<< "Common options:\n"
		<< "  --socket PATH       Acquisition daemon control socket\n"
		<< "  --shm NAME          Acquisition shared-memory ring name\n"
		<< "  --timeout-ms MS     Initial data/daemon timeout (default: 3000)\n";
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

std::vector<std::size_t> parse_channels(const std::string &value)
{
	std::vector<std::size_t> channels;
	std::size_t begin = 0;
	while (begin <= value.size()) {
		const auto end = value.find(',', begin);
		const auto token = value.substr(begin, end - begin);
		std::size_t parsed = 0;
		unsigned long channel = 0;
		try {
			channel = std::stoul(token, &parsed, 10);
		} catch (const std::exception &) {
			throw std::invalid_argument(
				"--channels requires comma-separated values 0-7");
		}
		if (parsed != token.size() || channel >= msap1::adc_channel_count ||
		    std::find(channels.begin(), channels.end(), channel) != channels.end())
			throw std::invalid_argument(
				"--channels requires unique values in the range 0-7");
		channels.push_back(static_cast<std::size_t>(channel));
		if (end == std::string::npos)
			break;
		begin = end + 1;
	}
	return channels;
}

Options parse_options(int argc, char **argv)
{
	if (argc < 2)
		throw std::invalid_argument("expected an adc-view, adc-health, adc-start, or adc-stop command");
	Options options;
	const std::string command = argv[1];
	if (command == "adc-view")
		options.command = Command::adc_view;
	else if (command == "adc-health")
		options.command = Command::adc_health;
	else if (command == "adc-start")
		options.command = Command::adc_start;
	else if (command == "adc-stop")
		options.command = Command::adc_stop;
	else if (command == "--help" || command == "-h") {
		usage(argv[0]);
		std::exit(0);
	} else
		throw std::invalid_argument("unknown command '" + command + "'");

	for (int index = 2; index < argc; ++index) {
		const std::string option = argv[index];
		if (option == "--help" || option == "-h") {
			usage(argv[0]);
			std::exit(0);
		}
		if (index + 1 >= argc)
			throw std::invalid_argument(option + " requires a value");
		const std::string value = argv[++index];
		if (option == "--rate") {
			const auto rate = parse_unsigned(value, option);
			if (options.command != Command::adc_view ||
			    rate > std::numeric_limits<std::uint32_t>::max())
				throw std::invalid_argument("invalid --rate for this command");
			options.display_rate_hz = static_cast<std::uint32_t>(rate);
		} else if (option == "--channels") {
			if (options.command != Command::adc_view)
				throw std::invalid_argument("--channels is only valid for adc-view");
			options.channels = parse_channels(value);
		} else if (option == "--format") {
			if (options.command != Command::adc_view)
				throw std::invalid_argument("--format is only valid for adc-view");
			(void)msap1::parse_output_format(value);
			options.format = value;
		} else if (option == "--output") {
			if (options.command != Command::adc_view)
				throw std::invalid_argument("--output is only valid for adc-view");
			options.output = value;
		} else if (option == "--frames") {
			if (options.command != Command::adc_view)
				throw std::invalid_argument("--frames is only valid for adc-view");
			options.frame_limit = parse_unsigned(value, option);
		} else if (option == "--duration") {
			if (options.command != Command::adc_view)
				throw std::invalid_argument("--duration is only valid for adc-view");
			options.duration_seconds = parse_duration(value);
		} else if (option == "--socket") {
			options.socket_path = value;
		} else if (option == "--shm") {
			options.shm_name = value;
		} else if (option == "--timeout-ms") {
			const auto timeout = parse_unsigned(value, option);
			if (timeout > static_cast<std::uint64_t>(
					      std::numeric_limits<int>::max()))
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

const char *yes_no(bool value)
{
	return value ? "yes" : "no";
}

const char *spi_error_text(std::uint32_t error)
{
	switch (error) {
	case MSAP1_ADC_SPI_HEALTH_OK: return "none";
	case MSAP1_ADC_SPI_HEALTH_NOT_INITIALIZED: return "AXI SPI was not initialized";
	case MSAP1_ADC_SPI_HEALTH_TRANSFER_FAILED: return "SPI transfer failed";
	case MSAP1_ADC_SPI_HEALTH_PROTOCOL_FAILED: return "AD7771 response header was invalid";
	default: return "internal RPU error";
	}
}

void require_daemon_ok(const msap1::AcquisitionResponse &response)
{
	if (response.status != msap1::AcquisitionStatus::ok)
		throw std::runtime_error("acquisition daemon request failed (status " +
					 std::to_string(static_cast<std::uint32_t>(response.status)) + ")");
}

int run_adc_health(const Options &options)
{
	msap1::AcquisitionClient client(options.socket_path);
	const auto response = client.request(msap1::AcquisitionCommand::health,
					     options.timeout_ms);
	require_daemon_ok(response);
	const auto &health = response.rpu_health;
	const bool spi_ok = health_flag(health, MSAP1_ADC_HEALTH_SPI_RESPONSIVE);
	const bool initialized = health_flag(health, MSAP1_ADC_HEALTH_INITIALIZED);
	const bool init_complete = health_flag(health, MSAP1_ADC_HEALTH_INIT_COMPLETE);
	const bool config_match = health_flag(health, MSAP1_ADC_HEALTH_CONFIG_MATCH);
	const bool capture_active = health_flag(health, MSAP1_ADC_HEALTH_CAPTURE_ACTIVE);
	const bool no_overflow = health_flag(health, MSAP1_ADC_HEALTH_NO_OVERFLOW);
	const bool headers_valid = health_flag(health, MSAP1_ADC_HEALTH_HEADERS_VALID);
	const bool linux_ok = response.running != 0u &&
		response.published_sequence != 0u && response.iio_read_errors == 0u;
	const bool healthy = linux_ok && spi_ok && initialized && init_complete &&
		config_match && capture_active && no_overflow && headers_valid;
	const double header_error_percent = health.frame_count == 0u ? 0.0 :
		100.0 * static_cast<double>(health.header_error_count) /
		static_cast<double>(health.frame_count);

	std::cout << "AD7771 acquisition health: " << (healthy ? "PASS" : "FAIL") << '\n'
		<< "  Linux acquisition:    " << yes_no(response.running != 0u) << '\n'
		<< "  IIO frames:           " << response.published_sequence << '\n'
		<< "  IIO bytes:            " << response.iio_bytes << '\n'
		<< "  DMA blocks:           " << response.iio_blocks << '\n'
		<< "  IIO read errors:      " << response.iio_read_errors << '\n'
		<< "  SPI responsive:       " << yes_no(spi_ok) << '\n'
		<< "  SPI error:            " << spi_error_text(health.spi_error) << '\n'
		<< "  ADC initialized:      " << yes_no(initialized) << '\n'
		<< "  INIT_COMPLETE:        " << yes_no(init_complete) << '\n'
		<< "  Configuration match:  " << yes_no(config_match) << '\n'
		<< "  Sample rate:          " << health.sample_rate_hz << " frame/s\n"
		<< "  Expected decimation:  " << health.expected_decimation << '\n'
		<< "  Capture active:       " << yes_no(capture_active) << '\n'
		<< "  Capture flags:        0x" << std::hex << std::setw(8)
		<< std::setfill('0') << health.capture_flags << std::dec
		<< std::setfill(' ') << '\n'
		<< "  PL frames:            " << health.frame_count << '\n'
		<< "  PL packets:           " << health.packet_count << '\n'
		<< "  FIFO overflows:       " << health.overflow_count << '\n'
		<< "  Header errors:        " << health.header_error_count << " ("
		<< std::fixed << std::setprecision(3) << header_error_percent << "%)\n"
		<< "  ADC alerts:           " << health.alert_count << '\n'
		<< "  Registers: STATUS_3=0x" << std::hex << std::setfill('0')
		<< std::setw(2) << static_cast<unsigned>(health.status_3)
		<< " CONFIG1=0x" << std::setw(2)
		<< static_cast<unsigned>(health.general_user_config_1)
		<< " CONFIG2=0x" << std::setw(2)
		<< static_cast<unsigned>(health.general_user_config_2)
		<< " CONFIG3=0x" << std::setw(2)
		<< static_cast<unsigned>(health.general_user_config_3)
		<< " DOUT_FORMAT=0x" << std::setw(2)
		<< static_cast<unsigned>(health.dout_format) << '\n'
		<< "             SRC_N_MSB=0x" << std::setw(2)
		<< static_cast<unsigned>(health.src_n_msb)
		<< " SRC_N_LSB=0x" << std::setw(2)
		<< static_cast<unsigned>(health.src_n_lsb)
		<< " SRC_IF_MSB=0x" << std::setw(2)
		<< static_cast<unsigned>(health.src_if_msb)
		<< " SRC_IF_LSB=0x" << std::setw(2)
		<< static_cast<unsigned>(health.src_if_lsb)
		<< " SRC_UPDATE=0x" << std::setw(2)
		<< static_cast<unsigned>(health.src_update)
		<< std::dec << std::setfill(' ') << '\n';
	return healthy ? 0 : 1;
}

int run_adc_view(const Options &options)
{
	msap1::AcquisitionClient client(options.socket_path);
	const auto info = client.request(msap1::AcquisitionCommand::info,
					 options.timeout_ms);
	require_daemon_ok(info);
	if (info.running == 0u)
		throw std::runtime_error("FPGA acquisition is stopped; run adc-start");

	msap1::SharedRingReader ring(options.shm_name);
	const auto sample_rate = ring.sample_rate_hz();
	if (options.display_rate_hz > sample_rate ||
	    sample_rate % options.display_rate_hz != 0u)
		throw std::invalid_argument("--rate must be a divisor of the ADC sample rate");
	const auto stride = sample_rate / options.display_rate_hz;

	std::unique_ptr<std::ofstream> file;
	std::ostream *output = &std::cout;
	if (!options.output.empty()) {
		file = std::make_unique<std::ofstream>(options.output);
		if (!*file)
			throw std::runtime_error("cannot open output file '" + options.output + "'");
		output = file.get();
	}
	msap1::Visualizer visualizer(*output,
		msap1::parse_output_format(options.format), options.channels);
	const auto started_at = Clock::now();
	const auto data_deadline = started_at +
		std::chrono::milliseconds(options.timeout_ms);
	std::uint64_t cursor = ring.published_sequence();
	std::uint64_t dropped = 0;
	std::uint64_t reported_dropped = 0;
	std::uint64_t displayed = 0;

	while (!stop_requested) {
		if (options.duration_seconds &&
		    std::chrono::duration<double>(Clock::now() - started_at).count() >=
			    *options.duration_seconds)
			break;
		if (options.frame_limit && displayed >= *options.frame_limit)
			break;

		msap1::AdcSampleFrame frame{};
		if (!ring.read(cursor, frame, dropped)) {
			if (!ring.running())
				throw std::runtime_error("FPGA acquisition stopped");
			if (displayed == 0 && Clock::now() >= data_deadline)
				throw std::runtime_error("timed out waiting for IIO sample data");
			std::this_thread::sleep_for(1ms);
			continue;
		}
		const auto frame_sequence = cursor - 1;
		cursor = frame_sequence + stride;
		if (dropped != reported_dropped) {
			std::cerr << "adc-view: shared-ring overrun, dropped "
				  << (dropped - reported_dropped) << " frame(s)\n";
			reported_dropped = dropped;
		}

		msap1::AdcBatch batch;
		batch.adc_sample_rate_hz = sample_rate;
		batch.display_rate_hz = options.display_rate_hz;
		batch.first_frame_index = frame_sequence;
		batch.capture_flags = ring.capture_flags();
		batch.frames.push_back(frame);
		visualizer.render(batch);
		++displayed;
	}
	visualizer.finish();
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
	case Command::adc_view: return run_adc_view(options);
	case Command::adc_health: return run_adc_health(options);
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
