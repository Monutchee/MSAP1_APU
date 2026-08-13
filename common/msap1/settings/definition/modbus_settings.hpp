#pragma once

#include <algorithm>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace msap1::settings {

struct ModbusTcpSettings {
	bool enabled = true;
	std::string listen_address = "0.0.0.0";
	std::uint16_t port = 502;
	std::uint32_t maximum_clients = 16;
	std::uint8_t unit_id = 1;
	bool operator==(const ModbusTcpSettings &) const = default;

	void validate() const
	{
		if (listen_address.empty())
			throw std::runtime_error("Modbus TCP listen address is empty");
		if (port == 0)
			throw std::runtime_error("Modbus TCP port must be nonzero");
		if (maximum_clients == 0 || maximum_clients > 1024)
			throw std::runtime_error(
				"Modbus TCP maximum clients must be 1..1024");
		if (unit_id == 0 || unit_id > 247)
			throw std::runtime_error("Modbus TCP unit id must be 1..247");
	}
};

struct ModbusRtuPortSettings {
	bool enabled = false;
	std::string device;
	std::uint32_t baud_rate = 115200;
	std::string parity = "none";
	std::uint8_t data_bits = 8;
	std::uint8_t stop_bits = 1;
	std::uint8_t unit_id = 1;
	bool operator==(const ModbusRtuPortSettings &) const = default;

	void validate() const
	{
		if (device.empty())
			throw std::runtime_error("Modbus RTU device path is empty");
		if (!device.starts_with("/dev/"))
			throw std::runtime_error(
				"Modbus RTU device must be below /dev");
		if (baud_rate < 1200 || baud_rate > 4'000'000)
			throw std::runtime_error(
				"Modbus RTU baud rate is outside 1200..4000000");
		if (parity != "none" && parity != "even" && parity != "odd")
			throw std::runtime_error(
				"Modbus RTU parity must be none, even, or odd");
		if (data_bits != 7 && data_bits != 8)
			throw std::runtime_error("Modbus RTU data bits must be 7 or 8");
		if (stop_bits != 1 && stop_bits != 2)
			throw std::runtime_error("Modbus RTU stop bits must be 1 or 2");
		if (unit_id == 0 || unit_id > 247)
			throw std::runtime_error("Modbus RTU unit id must be 1..247");
	}
};

struct ModbusSettings {
	bool enabled = true;
	ModbusTcpSettings tcp;
	std::vector<ModbusRtuPortSettings> rtu;
	bool operator==(const ModbusSettings &) const = default;

	void validate() const
	{
		if (enabled && tcp.enabled)
			tcp.validate();
		std::set<std::string> devices;
		for (const auto &port : rtu) {
			if (!port.enabled)
				continue;
			port.validate();
			if (!devices.insert(port.device).second)
				throw std::runtime_error(
					"duplicate Modbus RTU device " + port.device);
		}
		if (rtu.size() > 32)
			throw std::runtime_error(
				"at most 32 Modbus RTU ports may be configured");
	}
};

} // namespace msap1::settings
