#pragma once

/**
 * @file acquisition_service.hpp
 * @brief systemd service adapter around the CaptureCoordinator.
 */

#include "mnc/service/service.hpp"
#include "pipeline/capture_coordinator.hpp"
#include "support/options.hpp"

#include <atomic>
#include <exception>
#include <thread>

namespace msap1::acquisition::daemon {

/**
 * @brief Adapts the coordinator to the mnc::Service lifecycle.
 *
 * mnc::Service supplies readiness/watchdog/signal handling; this class only
 * maps its callbacks onto the coordinator: on_start synchronously completes
 * capture and IPC initialization before launching the poll loop, on_stop
 * requests loop exit and joins, and a worker failure is reported through
 * health() so systemd restarts the unit.
 */
class AcquisitionService final : public mnc::Service {
public:
	explicit AcquisitionService(const Options &options);

protected:
	void on_start() override;
	void on_reload() override;
	void on_stop() noexcept override;
	[[nodiscard]] mnc::ServiceHealth health() const override;

private:
	CaptureCoordinator coordinator_;
	std::thread worker_;
	std::exception_ptr failure_;
	std::atomic<bool> ready_{false};
	std::atomic<bool> failed_{false};
};

} // namespace msap1::acquisition::daemon
