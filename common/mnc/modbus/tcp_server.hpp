#pragma once

#include "mnc/modbus/request_handler.hpp"
#include "mnc/modbus/server.hpp"

#include <boost/asio/any_io_executor.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace mnc::modbus {

struct TcpServerConfig {
	std::string bind_address = "0.0.0.0";
	std::uint16_t port = 502;
	std::uint32_t maximum_clients = 16;
	std::uint8_t unit_id = 1;
};

/** Multi-client Modbus/TCP server using the common request handler. */
class ModbusTcpServer final : public ModbusTransport {
public:
	ModbusTcpServer(boost::asio::any_io_executor executor,
		RequestHandler &handler, TcpServerConfig config,
		ErrorHandler errors = {});
	~ModbusTcpServer() override;

	void start() override;
	void stop() noexcept override;
	[[nodiscard]] std::string_view name() const noexcept override
	{
		return "tcp";
	}
	[[nodiscard]] std::uint16_t local_port() const;

private:
	struct Impl;
	std::shared_ptr<Impl> impl_;
};

} // namespace mnc::modbus
