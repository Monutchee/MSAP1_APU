#include "msap1/acquisition/rpu/rpu_controller.hpp"

#include "mnc/logging/logging.hpp"

#include <array>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace msap1::acquisition {
namespace {

const mnc::logging::Logger rpmsg_log("fpga-acquisition", "rpmsg");

} // namespace

RpuController::RpuController(std::string service, std::string device)
	: endpoint_(std::move(service), std::move(device))
{
}

Message RpuController::transact(std::uint8_t type, const void *payload,
				std::size_t payload_size,
				std::chrono::milliseconds timeout)
{
	const auto sequence = ++next_sequence_;
	endpoint_.send(encode_request(type, sequence, payload, payload_size));
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	while (std::chrono::steady_clock::now() < deadline) {
		const auto remaining =
			std::chrono::duration_cast<std::chrono::milliseconds>(
				deadline - std::chrono::steady_clock::now());
		const auto frame = endpoint_.receive(remaining);
		if (frame.empty())
			continue;
		auto message = decode_message(frame.data(), frame.size());
		if (message.header.sequence != sequence)
			continue;
		if (message.header.type == MSAP1_RPU_MSG_ERROR ||
		    message.header.status != MSAP1_RPU_STATUS_OK) {
			const std::array fields{
				mnc::logging::Field{"MNC_REQUEST_ID",
						    std::to_string(sequence)},
				mnc::logging::Field{"MNC_RPU_STATUS",
						    std::to_string(
							    message.header.status)}};
			(void)rpmsg_log.write(
				mnc::logging::Priority::error,
				"RPU rejected request with status " +
					std::to_string(message.header.status),
				"request_rejected", fields);
			throw std::runtime_error(
				"RPU rejected request (status " +
				std::to_string(message.header.status) + ")");
		}
		return message;
	}
	const std::array timeout_fields{
		mnc::logging::Field{"MNC_REQUEST_ID", std::to_string(sequence)}};
	(void)rpmsg_log.write(mnc::logging::Priority::error,
		"timed out waiting for RPU response", "request_timeout",
		timeout_fields);
	throw std::runtime_error("timed out waiting for RPU response");
}

msap1_adc_health_payload RpuController::query_health()
{
	return decode_adc_health(transact(MSAP1_RPU_MSG_ADC_HEALTH_GET));
}

msap1_aggregation_health_payload RpuController::query_aggregation_health()
{
	return decode_aggregation_health(
		transact(MSAP1_RPU_MSG_AGGREGATION_HEALTH_GET));
}

} // namespace msap1::acquisition
