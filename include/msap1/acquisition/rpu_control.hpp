#ifndef MSAP1_ACQUISITION_RPU_CONTROL_HPP
#define MSAP1_ACQUISITION_RPU_CONTROL_HPP

#include "msap1/protocol.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace msap1::acquisition {

/** Product-facing RPU control contract used by the acquisition coordinator. */
class RpuControl {
public:
	virtual ~RpuControl() = default;

	[[nodiscard]] virtual Message transact(
		std::uint8_t type, const void *payload = nullptr,
		std::size_t payload_size = 0,
		std::chrono::milliseconds timeout =
			std::chrono::milliseconds{1000}) = 0;

	[[nodiscard]] virtual msap1_adc_health_payload query_health() = 0;
};

} // namespace msap1::acquisition

#endif
