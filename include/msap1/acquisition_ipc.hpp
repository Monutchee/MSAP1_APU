#ifndef MSAP1_ACQUISITION_IPC_HPP
#define MSAP1_ACQUISITION_IPC_HPP

#include "msap1/adc_sample.hpp"
#include "msap1/rpu_control_protocol.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace msap1 {

inline constexpr const char *acquisition_socket_path =
	"/run/monutchee/fpga-acquisition.sock";
inline constexpr const char *acquisition_shm_name =
	"/msap1-fpga-acquisition";
inline constexpr std::uint32_t acquisition_ring_capacity = 262144;

inline constexpr std::uint32_t acquisition_ipc_magic = 0x4d534151u;
inline constexpr std::uint16_t acquisition_ipc_version = 1;

enum class AcquisitionCommand : std::uint16_t {
	info = 1,
	health = 2,
	start = 3,
	stop = 4,
};

enum class AcquisitionStatus : std::uint32_t {
	ok = 0,
	bad_request = 1,
	not_running = 2,
	iio_error = 3,
	rpu_error = 4,
	internal_error = 5,
};

struct AcquisitionRequest {
	std::uint32_t magic = acquisition_ipc_magic;
	std::uint16_t version = acquisition_ipc_version;
	AcquisitionCommand command = AcquisitionCommand::info;
	std::uint64_t sequence = 0;
};

struct AcquisitionResponse {
	std::uint32_t magic = acquisition_ipc_magic;
	std::uint16_t version = acquisition_ipc_version;
	std::uint16_t reserved = 0;
	AcquisitionStatus status = AcquisitionStatus::ok;
	std::uint32_t running = 0;
	std::uint32_t sample_rate_hz = adc_default_sample_rate_hz;
	std::uint32_t channel_count = adc_channel_count;
	std::uint32_t frame_size = sizeof(AdcSampleFrame);
	std::uint32_t ring_capacity = acquisition_ring_capacity;
	std::uint32_t capture_flags = 0;
	std::uint64_t sequence = 0;
	std::uint64_t published_sequence = 0;
	std::uint64_t iio_bytes = 0;
	std::uint64_t iio_blocks = 0;
	std::uint64_t iio_read_errors = 0;
	msap1_adc_health_payload rpu_health{};
};

class AcquisitionClient {
public:
	explicit AcquisitionClient(
		std::string socket_path = acquisition_socket_path);
	AcquisitionResponse request(AcquisitionCommand command,
				    int timeout_ms = 3000) const;

private:
	std::string socket_path_;
};

} // namespace msap1

#endif
