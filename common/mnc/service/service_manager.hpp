#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace mnc {

enum class ServiceAction : std::uint8_t { start, stop, restart, reload };

enum class ServicePriorityTier : std::uint8_t {
	critical = 0,
	high = 1,
	normal = 2,
	background = 3,
};

[[nodiscard]] constexpr std::int32_t
service_priority_nice(ServicePriorityTier tier) noexcept
{
	switch (tier) {
	case ServicePriorityTier::critical: return -10;
	case ServicePriorityTier::high: return -5;
	case ServicePriorityTier::normal: return 0;
	case ServicePriorityTier::background: return 5;
	}
	return 0;
}

[[nodiscard]] constexpr std::string_view
service_priority_name(ServicePriorityTier tier) noexcept
{
	switch (tier) {
	case ServicePriorityTier::critical: return "critical";
	case ServicePriorityTier::high: return "high";
	case ServicePriorityTier::normal: return "normal";
	case ServicePriorityTier::background: return "background";
	}
	return "unknown";
}

struct ManagedService {
	std::string name;
	std::string unit;
	std::vector<std::string> after;
	/** Optional units are registered and controllable, but are not started by
	 * start_registered(). Product policy may activate them later. */
	bool auto_start = true;
	ServicePriorityTier priority_tier = ServicePriorityTier::normal;
};

struct ManagedServiceStatus {
	std::string name;
	std::string unit;
	std::string active_state;
	std::string sub_state;
	std::uint32_t restart_count = 0;
	bool permanently_failed = false;
	bool required_active = true;
	ServicePriorityTier priority_tier = ServicePriorityTier::normal;
	std::int32_t expected_nice = 0;
	std::int32_t effective_nice = 0;
	bool priority_matches = false;
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
