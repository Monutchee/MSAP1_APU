#include "support/meter_time_control.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace msap1::acquisition::daemon {
namespace {

#pragma pack(push, 1)
struct CorrelationIoctl {
	std::uint64_t tai_before_nanoseconds;
	std::uint64_t tai_after_nanoseconds;
	std::uint64_t realtime_before_nanoseconds;
	std::uint64_t realtime_after_nanoseconds;
	std::uint64_t pl_tick;
	std::uint64_t sample_index;
};

struct TenMinuteBoundaryIoctl {
	std::uint64_t target_sample_index;
	std::uint32_t valid;
	std::uint32_t reserved;
};

struct Frequency10sBoundaryIoctl {
	std::uint64_t start_sample_index;
	std::uint64_t end_sample_index;
	std::uint64_t utc_start_nanoseconds;
	std::uint64_t utc_end_nanoseconds;
	std::uint64_t utc_uncertainty_nanoseconds;
	std::uint32_t measured_sample_rate_millihz;
	std::uint32_t boundary_generation;
	std::uint32_t profile;
	std::uint32_t flags;
	std::uint32_t observer_status;
	std::uint32_t completed_count;
	std::uint32_t dropped_count;
	std::uint32_t overflow_count;
	std::uint32_t discontinuity_count;
	std::uint32_t reserved;
};
#pragma pack(pop)

static_assert(sizeof(CorrelationIoctl) == 48);
static_assert(sizeof(TenMinuteBoundaryIoctl) == 16);
static_assert(sizeof(Frequency10sBoundaryIoctl) == 80);

constexpr unsigned long correlate_ioctl = _IOR('T', 0x01, CorrelationIoctl);
constexpr unsigned long ten_minute_boundary_ioctl =
	_IOW('T', 0x02, TenMinuteBoundaryIoctl);
constexpr unsigned long frequency_10s_boundary_ioctl =
	_IOWR('T', 0x03, Frequency10sBoundaryIoctl);
constexpr unsigned long cancel_frequency_10s_ioctl = _IO('T', 0x04);
constexpr unsigned time_sync_attempts = 3u;
constexpr std::uint32_t boundary_valid = 1u << 0u;
constexpr std::uint32_t time_synchronized = 1u << 1u;

[[noreturn]] void throw_errno(const std::string &operation)
{
	throw std::runtime_error(operation + ": " + std::strerror(errno));
}

} // namespace

MeterTimeControl::MeterTimeControl(std::string device_path)
	: device_path_(std::move(device_path))
{
	fd_ = ::open(device_path_.c_str(), O_RDWR | O_CLOEXEC);
	if (fd_ < 0)
		throw_errno("open " + device_path_);
}

MeterTimeControl::~MeterTimeControl()
{
	if (fd_ >= 0)
		::close(fd_);
}

std::optional<MeterTimeSync> MeterTimeControl::time_sync() const noexcept
{
	std::optional<MeterTimeSync> best;
	for (unsigned attempt = 0; attempt < time_sync_attempts; ++attempt) {
		CorrelationIoctl sample{};
		if (::ioctl(fd_, correlate_ioctl, &sample) != 0 ||
		    sample.tai_after_nanoseconds < sample.tai_before_nanoseconds ||
		    sample.realtime_after_nanoseconds <
			sample.realtime_before_nanoseconds)
			continue;
		MeterTimeSync candidate{
			.sample_counter = sample.sample_index,
			.pl_tick = sample.pl_tick,
			.tai_nanoseconds = sample.tai_before_nanoseconds +
				(sample.tai_after_nanoseconds -
				 sample.tai_before_nanoseconds) / 2u,
			.realtime_nanoseconds = sample.realtime_before_nanoseconds +
				(sample.realtime_after_nanoseconds -
				 sample.realtime_before_nanoseconds) / 2u,
			.bracket_nanoseconds = sample.realtime_after_nanoseconds -
				sample.realtime_before_nanoseconds,
		};
		if (!best || candidate.bracket_nanoseconds < best->bracket_nanoseconds)
			best = candidate;
	}
	return best;
}

std::optional<msap1::WaveformCorrelation>
MeterTimeControl::waveform_correlation() const noexcept
{
	const auto sync = time_sync();
	if (!sync)
		return std::nullopt;
	return msap1::WaveformCorrelation{
		.tai_nanoseconds = sync->tai_nanoseconds,
		.pl_tick = sync->pl_tick,
		.frame_sequence = sync->sample_counter,
		.uncertainty_nanoseconds = sync->bracket_nanoseconds,
	};
}

void MeterTimeControl::program_ten_minute_boundary(
	std::uint64_t target_sample_index, bool valid)
{
	TenMinuteBoundaryIoctl request{target_sample_index, valid ? 1u : 0u, 0u};
	if (::ioctl(fd_, ten_minute_boundary_ioctl, &request) != 0)
		throw_errno("program ten-minute boundary");
}

Frequency10sObserverStatus MeterTimeControl::program_frequency_10s_boundary(
	const Frequency10sBoundary &boundary)
{
	const auto profile = static_cast<std::uint32_t>(
		boundary.nominal_frequency_hz) |
		(static_cast<std::uint32_t>(boundary.reference_channel) << 8u) |
		(static_cast<std::uint32_t>(boundary.filter_profile) << 16u) |
		(static_cast<std::uint32_t>(boundary.calibration_profile) << 24u);
	Frequency10sBoundaryIoctl request{
		boundary.start_sample_index,
		boundary.end_sample_index,
		boundary.utc_start_nanoseconds,
		boundary.utc_end_nanoseconds,
		boundary.utc_uncertainty_nanoseconds,
		boundary.measured_sample_rate_millihz,
		boundary.boundary_generation,
		profile,
		(boundary.valid ? boundary_valid : 0u) |
			(boundary.time_synchronized ? time_synchronized : 0u),
		0u, 0u, 0u, 0u, 0u, 0u,
	};
	if (::ioctl(fd_, frequency_10s_boundary_ioctl, &request) != 0)
		throw_errno("program frequency ten-second boundary");
	return {
		.status = request.observer_status,
		.completed_count = request.completed_count,
		.dropped_count = request.dropped_count,
		.overflow_count = request.overflow_count,
		.discontinuity_count = request.discontinuity_count,
	};
}

void MeterTimeControl::cancel_frequency_10s_boundary()
{
	if (::ioctl(fd_, cancel_frequency_10s_ioctl) != 0)
		throw_errno("cancel frequency ten-second boundary");
}

} // namespace msap1::acquisition::daemon
