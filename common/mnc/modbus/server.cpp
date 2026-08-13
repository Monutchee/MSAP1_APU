#include "mnc/modbus/server.hpp"

#include <stdexcept>
#include <utility>

namespace mnc::modbus {

void ModbusServer::add(std::unique_ptr<ModbusTransport> transport)
{
	if (!transport)
		throw std::invalid_argument("Modbus transport is null");
	if (started_ != 0)
		throw std::logic_error("cannot add a running Modbus transport");
	transports_.push_back(std::move(transport));
}

void ModbusServer::start()
{
	if (started_ != 0)
		return;
	try {
		for (auto &transport : transports_) {
			transport->start();
			++started_;
		}
	} catch (...) {
		stop();
		throw;
	}
}

void ModbusServer::stop() noexcept
{
	while (started_ != 0) {
		--started_;
		transports_[started_]->stop();
	}
}

} // namespace mnc::modbus
