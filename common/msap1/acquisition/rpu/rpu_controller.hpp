#ifndef MSAP1_ACQUISITION_RPU_CONTROLLER_HPP
#define MSAP1_ACQUISITION_RPU_CONTROLLER_HPP

#include "msap1/acquisition/rpu/rpu_control.hpp"
#include "msap1/acquisition/rpu/rpmsg_endpoint.hpp"

#include <string>

namespace msap1::acquisition {

/**
 * Serialized request/response adapter for R5 core 0.
 *
 * Sequence matching, timeout handling, and RPU status validation are kept out
 * of service orchestration. The class owns the RPMsg endpoint exclusively.
 */
class RpuController final : public RpuControl {
public:
	RpuController(std::string service, std::string device = {});

	[[nodiscard]] Message transact(
		std::uint8_t type, const void *payload = nullptr,
		std::size_t payload_size = 0,
		std::chrono::milliseconds timeout =
			std::chrono::milliseconds{1000}) override;
	[[nodiscard]] msap1_adc_health_payload query_health() override;

	[[nodiscard]] const std::string &device_path() const noexcept
	{
		return endpoint_.device_path();
	}

private:
	RpmsgEndpoint endpoint_;
	std::uint32_t next_sequence_ = 0x90000000u;
};

} // namespace msap1::acquisition

#endif
