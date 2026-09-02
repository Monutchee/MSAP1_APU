#include "support/options.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace msap1::acquisition::daemon {

namespace {

void usage(const char *program)
{
	std::cerr
		<< "Usage: " << program << " [options]\n"
		<< "  --service NAME       RPMsg service (default: mncos-r5c0-ctrl)\n"
		<< "  --rpmsg-device PATH  Use an existing /dev/rpmsgN endpoint\n"
		<< "  --aggregation-service NAME R5C1 diagnostic service (default: mncos-r5c1-ctrl)\n"
		<< "  --aggregation-rpmsg-device PATH Use an existing R5C1 /dev/rpmsgN endpoint\n"
		<< "  --meter-device PATH  Meter DMA device (default: /dev/msap1-meter)\n"
		<< "  --waveform-device PATH Waveform DMA device (default: /dev/msap1-waveform)\n"
		<< "  --meter-time-device PATH Time-control device (default: /dev/meter-time)\n"
		<< "  --waveform-directory PATH Completed waveform storage\n"
		<< "  --socket PATH        Control socket path\n";
}

} // namespace

Options parse_options(int argc, char **argv)
{
	Options options;
	for (int index = 1; index < argc; ++index) {
		const std::string option = argv[index];
		if (option == "--help" || option == "-h") {
			usage(argv[0]);
			std::exit(0);
		}
		if (index + 1 >= argc)
			throw std::invalid_argument(option + " requires a value");
		const std::string value = argv[++index];
		if (option == "--service")
			options.service = value;
		else if (option == "--rpmsg-device")
			options.rpmsg_device = value;
		else if (option == "--aggregation-service")
			options.aggregation_service = value;
		else if (option == "--aggregation-rpmsg-device")
			options.aggregation_rpmsg_device = value;
		else if (option == "--meter-device")
			options.meter_device = value;
		else if (option == "--waveform-device")
			options.waveform_device = value;
		else if (option == "--meter-time-device")
			options.meter_time_device = value;
		else if (option == "--waveform-directory")
			options.waveform_directory = value;
		else if (option == "--socket")
			options.socket_path = value;
		else
			throw std::invalid_argument("unknown option '" + option + "'");
	}
	return options;
}

} // namespace msap1::acquisition::daemon
