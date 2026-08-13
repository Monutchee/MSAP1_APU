#include "modbus_service.hpp"

#include "mnc/logging/logging.hpp"

#include <exception>
#include <string>

int main()
{
	const mnc::logging::Logger lifecycle_log{"modbus", "lifecycle"};
	try {
		msap1::modbus::daemon::ModbusService service;
		return service.execute();
	} catch (const std::exception &error) {
		(void)lifecycle_log.write(mnc::logging::Priority::critical,
			"msap1-modbus-server failed to start: " +
				std::string(error.what()),
			"service_failed");
		return 1;
	} catch (...) {
		(void)lifecycle_log.write(mnc::logging::Priority::critical,
			"msap1-modbus-server failed to start: unknown exception",
			"service_failed");
		return 1;
	}
}
