#pragma once

#include "mnc/logging/logging.hpp"

#include <atomic>
#include <chrono>
#include <string>
#include <string_view>

namespace mnc {

/** Snapshot returned by a managed service to systemd and ServiceManager. */
struct ServiceHealth {
	bool healthy = true;
	std::string summary = "healthy";
};

/**
 * Common lifecycle boundary for long-running Monutchee processes.
 *
 * Derived on_start() functions start their asynchronous work and return.
 * execute() owns signal handling, sd_notify readiness/watchdog messages,
 * exception containment and exactly-once orderly shutdown.
 */
class Service {
public:
	Service(std::string name, std::string component);
	virtual ~Service() = default;

	Service(const Service &) = delete;
	Service &operator=(const Service &) = delete;

	int execute();
	[[nodiscard]] std::string_view name() const noexcept { return name_; }

protected:
	virtual void on_start() = 0;
	virtual void on_reload() = 0;
	virtual void on_stop() noexcept = 0;
	[[nodiscard]] virtual ServiceHealth health() const = 0;

	void request_stop() noexcept;
	/** Request the same orderly reload path used by SIGHUP. */
	void request_reload() noexcept;
	[[nodiscard]] bool stop_requested() const noexcept;
	[[nodiscard]] logging::Logger &logger() noexcept { return logger_; }

private:
	bool notify(std::string_view state) const noexcept;

	std::string name_;
	logging::Logger logger_;
	std::atomic<bool> stop_requested_{false};
	std::atomic<bool> reload_requested_{false};
};

} // namespace mnc
