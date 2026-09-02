#include "acquisition_service.hpp"

#include <string>

namespace msap1::acquisition::daemon {

AcquisitionService::AcquisitionService(const Options &options)
	: Service("MSAP1 FPGA acquisition", "fpga-acquisition"),
	  coordinator_(options)
{
}

void AcquisitionService::on_start()
{
	coordinator_.clear_stop_request();
	/* This synchronous boundary is what mnc::Service turns into READY=1.
	 * Archive CRC validation is already running in the background, so only
	 * capture-critical DMA/RPU setup and the IPC bind delay readiness. */
	coordinator_.initialize();
	worker_ = std::thread([this] {
		try {
			coordinator_.run();
		} catch (...) {
			failure_ = std::current_exception();
			failed_ = true;
			request_stop();
		}
	});
	ready_ = true;
}

void AcquisitionService::on_reload()
{
	(void)logger().write(mnc::logging::Priority::info,
		"acquisition configuration reload requested; runtime "
		"configuration remains transaction-controlled",
		"reload_deferred");
}

void AcquisitionService::on_stop() noexcept
{
	ready_ = false;
	coordinator_.request_stop();
	if (worker_.joinable())
		worker_.join();
	if (!failure_)
		return;
	try {
		std::rethrow_exception(failure_);
	} catch (const std::exception &error) {
		(void)logger().write(mnc::logging::Priority::critical,
			"acquisition worker failed: " +
				std::string(error.what()),
			"worker_failed");
	} catch (...) {
		(void)logger().write(mnc::logging::Priority::critical,
			"acquisition worker failed", "worker_failed");
	}
}

mnc::ServiceHealth AcquisitionService::health() const
{
	if (failed_.load())
		return {false, "acquisition worker failed"};
	return ready_.load()
		? mnc::ServiceHealth{true, "acquisition running"}
		: mnc::ServiceHealth{false, "acquisition is not ready"};
}

} // namespace msap1::acquisition::daemon
