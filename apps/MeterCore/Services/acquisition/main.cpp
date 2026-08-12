/**
 * @file main.cpp
 * @brief Entry point of msap1-fpga-acquisition.
 *
 * The daemon is decomposed by responsibility:
 *  - support/   — options, loggers, clock and eventfd utilities
 *  - config/    — settings-authority loading and wire/metadata translation
 *  - pipeline/  — CaptureCoordinator (lifecycle + transactions + poll loop),
 *                 MeterRecordIngestor (DMA -> validate -> persist -> publish),
 *                 RpuHealthMonitor (cached RPU audits with confirmation)
 *  - ipc/       — IpcChannel (socket transport) and the command handlers
 *
 * main() only parses options and hands the coordinator to the mnc::Service
 * shell in acquisition_service.cpp.
 */

#include "acquisition_service.hpp"
#include "support/logs.hpp"
#include "support/options.hpp"

#include <exception>
#include <string>

int main(int argc, char **argv)
{
	using namespace msap1::acquisition::daemon;
	try {
		AcquisitionService service(parse_options(argc, argv));
		return service.execute();
	} catch (const std::exception &error) {
		log_message(lifecycle_log, mnc::logging::Priority::critical,
			"msap1-fpga-acquisition: " + std::string(error.what()),
			"service_failed");
		return 1;
	}
}
