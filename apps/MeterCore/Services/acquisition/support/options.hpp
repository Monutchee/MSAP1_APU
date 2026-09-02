#pragma once

/**
 * @file options.hpp
 * @brief Command-line options of the acquisition daemon.
 */

#include "msap1/acquisition/ipc/acquisition_commands.hpp"

#include <string>

namespace msap1::acquisition::daemon {

/** Runtime paths and endpoint names, overridable from the command line. */
struct Options {
	/** RPMsg control service announced by R5 core 0. */
	std::string service = "mncos-r5c0-ctrl";
	/** Pre-bound /dev/rpmsgN endpoint; discovered from service when empty. */
	std::string rpmsg_device;
	/** RPMsg diagnostic service announced by R5 core 1. */
	std::string aggregation_service = "mncos-r5c1-ctrl";
	/** Optional pre-bound endpoint for R5C1 aggregation diagnostics. */
	std::string aggregation_rpmsg_device;
	/** Meter record DMA character device. */
	std::string meter_device = "/dev/msap1-meter";
	/** Waveform block DMA character device. */
	std::string waveform_device = "/dev/msap1-waveform";
	/** Independent metrology time-control character device. */
	std::string meter_time_device = "/dev/meter-time";
	/** Directory receiving completed .mncwf captures. */
	std::string waveform_directory = "/data/mnc/waveform";
	/** Unix control socket serving the acquisition command IPC. */
	std::string socket_path = msap1::acquisition_socket_path;
};

/**
 * @brief Parse argv into Options.
 *
 * Prints usage and exits for --help; throws std::invalid_argument for an
 * unknown option or a missing value.
 */
Options parse_options(int argc, char **argv);

} // namespace msap1::acquisition::daemon
