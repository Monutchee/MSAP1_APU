#include "msap1/protocol.hpp"
#include "msap1/rpmsg_endpoint.hpp"
#include "msap1/visualizer.hpp"

#include <csignal>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

volatile std::sig_atomic_t stop_requested = 0;

void handle_signal(int)
{
	stop_requested = 1;
}

struct Options {
	std::string service = "mncos-r5c0-ctrl";
	std::string device;
	std::string format = "terminal";
	std::string output;
	std::uint32_t display_rate_hz = 1000;
	std::optional<std::uint64_t> frame_limit;
	std::optional<double> duration_seconds;
	int timeout_ms = 3000;
};

void usage(const char *program)
{
	std::cerr
		<< "Usage:\n  " << program << " adc-view [options]\n\n"
		<< "Options:\n"
		<< "  --rate HZ           Display stream rate (default: 1000)\n"
		<< "  --format FORMAT     terminal, table, csv, or jsonl\n"
		<< "  --output FILE       Write visualization/export to FILE\n"
		<< "  --frames COUNT      Stop after COUNT displayed frames\n"
		<< "  --duration SECONDS  Stop after elapsed SECONDS\n"
		<< "  --service NAME      RPMsg service (default: mncos-r5c0-ctrl)\n"
		<< "  --device PATH       Use an existing /dev/rpmsgN endpoint\n"
		<< "  --timeout-ms MS     Initial reply/data timeout (default: 3000)\n";
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
	if (argc == 2 && (std::string(argv[1]) == "--help" ||
			  std::string(argv[1]) == "-h")) {
		usage(argv[0]);
		std::exit(0);
	}
	if (argc < 2 || std::string(argv[1]) != "adc-view")
		throw std::invalid_argument("expected the adc-view command");

	Options options;
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
			if (rate > std::numeric_limits<std::uint32_t>::max())
				throw std::invalid_argument("--rate is too large");
			options.display_rate_hz = static_cast<std::uint32_t>(rate);
		} else if (option == "--format") {
			(void)msap1::parse_output_format(value);
			options.format = value;
		} else if (option == "--output") {
			options.output = value;
		} else if (option == "--frames") {
			options.frame_limit = parse_unsigned(value, option);
		} else if (option == "--duration") {
			options.duration_seconds = parse_duration(value);
		} else if (option == "--service") {
			options.service = value;
		} else if (option == "--device") {
			options.device = value;
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
	if (options.service.empty())
		throw std::invalid_argument("--service cannot be empty");
	return options;
}

std::string status_text(std::uint32_t status)
{
	switch (status) {
	case MSAP1_RPU_STATUS_OK:
		return "ok";
	case MSAP1_RPU_STATUS_BAD_PAYLOAD:
		return "unsupported display rate or bad request";
	case MSAP1_RPU_STATUS_ADC_UNAVAILABLE:
		return "ADC capture is unavailable on the RPU";
	case MSAP1_RPU_STATUS_BAD_VERSION:
		return "protocol version mismatch";
	case MSAP1_RPU_STATUS_INTERNAL_ERROR:
		return "RPU internal error";
	default:
		return "RPU status " + std::to_string(status);
	}
}

void wait_for_ack(msap1::RpmsgEndpoint &endpoint, std::uint32_t sequence,
		  std::chrono::milliseconds timeout)
{
	const auto deadline = Clock::now() + timeout;
	while (Clock::now() < deadline) {
		const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
			deadline - Clock::now());
		const auto frame = endpoint.receive(remaining);
		if (frame.empty())
			continue;
		const auto message = msap1::decode_message(frame.data(), frame.size());
		if (message.header.sequence != sequence)
			continue;
		if (message.header.type == MSAP1_RPU_MSG_ERROR ||
		    message.header.status != MSAP1_RPU_STATUS_OK)
			throw std::runtime_error(status_text(message.header.status));
		if (message.header.type == MSAP1_RPU_MSG_ACK)
			return;
		// A final sample batch may already be queued before a STOP ACK.
	}
	throw std::runtime_error("timed out waiting for the RPU acknowledgement");
}

void stop_stream(msap1::RpmsgEndpoint &endpoint, std::uint32_t sequence) noexcept
{
	try {
		endpoint.send(msap1::encode_request(MSAP1_RPU_MSG_ADC_STREAM_STOP,
						    sequence));
		wait_for_ack(endpoint, sequence, 500ms);
	} catch (const std::exception &) {
		// Endpoint teardown also makes the RPU discard the subscriber.
	}
}

int run(int argc, char **argv)
{
	const auto options = parse_options(argc, argv);
	const auto format = msap1::parse_output_format(options.format);

	std::unique_ptr<std::ofstream> file;
	std::ostream *output = &std::cout;
	if (!options.output.empty()) {
		file = std::make_unique<std::ofstream>(options.output);
		if (!*file)
			throw std::runtime_error("cannot open output file '" +
						 options.output + "'");
		output = file.get();
	}

	msap1::RpmsgEndpoint endpoint(options.service, options.device);
	std::cerr << "Connected to " << options.service << " through "
		  << endpoint.device_path() << '\n';

	msap1_adc_stream_request request{};
	request.display_rate_hz = options.display_rate_hz;
	constexpr std::uint32_t start_sequence = 0x80000001u;
	constexpr std::uint32_t stop_sequence = 0x80000002u;
	endpoint.send(msap1::encode_request(MSAP1_RPU_MSG_ADC_STREAM_START,
						    start_sequence, &request,
						    sizeof(request)));
	wait_for_ack(endpoint, start_sequence,
		     std::chrono::milliseconds(options.timeout_ms));

	msap1::Visualizer visualizer(*output, format);
	const auto started_at = Clock::now();
	auto data_deadline = started_at +
		std::chrono::milliseconds(options.timeout_ms);
	std::uint64_t displayed_frames = 0;
	bool received_data = false;

	try {
		while (!stop_requested) {
			if (options.duration_seconds &&
			    std::chrono::duration<double>(Clock::now() - started_at).count() >=
				    *options.duration_seconds)
				break;
			if (options.frame_limit && displayed_frames >= *options.frame_limit)
				break;

			const auto frame = endpoint.receive(250ms);
			if (frame.empty()) {
				if (!received_data && Clock::now() >= data_deadline)
					throw std::runtime_error(
						"timed out waiting for ADC sample data");
				continue;
			}
			const auto message = msap1::decode_message(frame.data(), frame.size());
			if (message.header.type == MSAP1_RPU_MSG_ERROR)
				throw std::runtime_error(status_text(message.header.status));
			if (message.header.type != MSAP1_RPU_MSG_ADC_SAMPLE_BATCH)
				continue;

			auto batch = msap1::decode_adc_batch(message);
			received_data = true;
			if (options.frame_limit) {
				const auto remaining = *options.frame_limit - displayed_frames;
				if (batch.frames.size() > remaining)
					batch.frames.resize(static_cast<std::size_t>(remaining));
			}
			displayed_frames += batch.frames.size();
			visualizer.render(batch);
		}
	} catch (...) {
		stop_stream(endpoint, stop_sequence);
		visualizer.finish();
		throw;
	}

	stop_stream(endpoint, stop_sequence);
	visualizer.finish();
	return 0;
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
