#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace mnc {

enum class ServiceAction : std::uint8_t { start, stop, restart, reload };

struct ManagedService {
	std::string name;
	std::string unit;
	std::vector<std::string> after;
};

struct ManagedServiceStatus {
	std::string name;
	std::string unit;
	std::string active_state;
	std::string sub_state;
	std::uint32_t restart_count = 0;
	bool permanently_failed = false;
};

class UnitController {
public:
	virtual ~UnitController() = default;
	virtual ManagedServiceStatus inspect(const ManagedService &service) = 0;
	virtual void control(const ManagedService &service, ServiceAction action) = 0;
};

/** systemd sd-bus controller used by the target service-manager process. */
std::shared_ptr<UnitController> make_systemd_unit_controller();

/**
 * Product-neutral registry and dependency-order coordinator.
 *
 * systemd remains responsible for child PIDs and restart limits. This class
 * orders initial activation, adopts active units, and presents coherent
 * health/control state to IPC clients.
 */
class ServiceManager {
public:
	explicit ServiceManager(std::shared_ptr<UnitController> controller);

	void register_service(ManagedService service);
	void start_registered();
	[[nodiscard]] std::vector<ManagedServiceStatus> statuses();
	[[nodiscard]] ManagedServiceStatus status(std::string_view name);
	[[nodiscard]] ManagedServiceStatus control(std::string_view name,
						   ServiceAction action);

private:
	ManagedService find(std::string_view name) const;
	void start_one(const ManagedService &service,
		       std::vector<std::string> &started);

	std::shared_ptr<UnitController> controller_;
	mutable std::mutex mutex_;
	std::vector<ManagedService> services_;
};

} // namespace mnc
