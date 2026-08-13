#pragma once

#include "mnc/modbus/request_handler.hpp"
#include "mnc/modbus/server.hpp"

#include <boost/asio/any_io_executor.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mnc::modbus {

enum class SerialParity : std::uint8_t { none, even, odd };

struct RtuPortConfig {
	std::string device;
	std::uint32_t baud_rate = 19200;
	std::uint8_t data_bits = 8;
	SerialParity parity = SerialParity::even;
	std::uint8_t stop_bits = 1;
	std::uint8_t unit_id = 1;
};

/**
 * Incremental RTU request assembler.
 *
 * Supported requests use their defined wire length, allowing fragmented reads
 * and multiple complete requests in one kernel read. Unsupported functions do
 * not have a generally inferable request length and remain buffered until the
 * serial-line silent interval supplies the authoritative frame boundary.
 */
class RtuFrameAssembler final {
public:
	[[nodiscard]] std::vector<std::vector<std::byte>> push(
		std::span<const std::byte> bytes);
	/** Finish the pending frame when the Modbus RTU t3.5 gap expires. */
	[[nodiscard]] std::optional<std::vector<std::byte>> finish_on_silence();
	void clear() noexcept { buffer_.clear(); }
	[[nodiscard]] bool empty() const noexcept { return buffer_.empty(); }
	[[nodiscard]] std::size_t pending_size() const noexcept
	{
		return buffer_.size();
	}

private:
	[[nodiscard]] static std::optional<std::size_t> frame_size(
		std::span<const std::byte> bytes);
	std::vector<std::byte> buffer_;
};

[[nodiscard]] std::uint16_t crc16(std::span<const std::byte> bytes);
[[nodiscard]] bool valid_crc(std::span<const std::byte> frame);

/** One asynchronous RTU session per configured serial port. */
class ModbusRtuServer final : public ModbusTransport {
public:
	ModbusRtuServer(boost::asio::any_io_executor executor,
		RequestHandler &handler, std::vector<RtuPortConfig> ports,
		ErrorHandler errors = {});
	~ModbusRtuServer() override;

	void start() override;
	void stop() noexcept override;
	[[nodiscard]] std::string_view name() const noexcept override
	{
		return "rtu";
	}

private:
	struct Impl;
	std::shared_ptr<Impl> impl_;
};

} // namespace mnc::modbus
