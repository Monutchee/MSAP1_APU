#include "modbus_service.hpp"

int main()
{
	try {
		msap1::modbus::daemon::ModbusService service;
		return service.execute();
	} catch (...) {
		return 1;
	}
}
