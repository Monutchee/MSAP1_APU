#pragma once

#include "mnc/datalogger/meter_data_content_writer.hpp"

#include <chrono>

namespace mnc::datalogger {

enum class DeliveryDisposition : std::uint8_t {
	Succeeded,
	Retryable,
	Blocked,
};

struct DeliveryResult {
	DeliveryDisposition disposition = DeliveryDisposition::Retryable;
	std::string remote_result;
	std::string sanitized_error;
};

struct DeliveryRequest {
	std::string channel_id;
	GeneratedContent content;
	bool zero_data_probe = false;
};

class DataChannel {
public:
	virtual ~DataChannel() = default;
	[[nodiscard]] virtual std::string_view protocol() const noexcept = 0;
	[[nodiscard]] virtual DeliveryResult deliver(
		const DeliveryRequest &request) = 0;
};

} // namespace mnc::datalogger
