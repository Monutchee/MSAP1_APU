#pragma once

#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

namespace mnc::modbus {

/** Lifecycle implemented by each wire transport. */
class ModbusTransport {
public:
	virtual ~ModbusTransport() = default;
	virtual void start() = 0;
	virtual void stop() noexcept = 0;
	[[nodiscard]] virtual std::string_view name() const noexcept = 0;
};

/**
 * High-level owner for a shared handler and any number of transports.
 *
 * This small Composite keeps product code independent of whether TCP, one
 * RTU transport, or both are enabled. A partially failed start is rolled
 * back in reverse order.
 */
class ModbusServer final {
public:
	void add(std::unique_ptr<ModbusTransport> transport);
	void start();
	void stop() noexcept;
	[[nodiscard]] bool empty() const noexcept { return transports_.empty(); }

private:
	std::vector<std::unique_ptr<ModbusTransport>> transports_;
	std::size_t started_ = 0;
};

} // namespace mnc::modbus
