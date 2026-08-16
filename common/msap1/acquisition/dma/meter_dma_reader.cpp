#include "msap1/acquisition/dma/meter_dma_reader.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <utility>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace msap1::acquisition {
namespace {

[[noreturn]] void throw_errno(const std::string &operation)
{
	throw std::runtime_error(operation + ": " + std::strerror(errno));
}

/* Mirror of struct msap1_dma_transport_status (msap1_dma_uapi.h); the layout
 * and request number are frozen kernel ABI. */
struct MeterTransportStatusIoctl {
	std::uint64_t produced_blocks;
	std::uint64_t consumed_blocks;
	std::uint64_t overrun_blocks;
	std::uint32_t ring_blocks;
	/* Cyclic completion callbacks the driver saw; diagnostic only.
	 * produced_blocks - callbacks is the completion-coalescing deficit. */
	std::uint32_t callbacks;
};
static_assert(sizeof(MeterTransportStatusIoctl) == 32);

constexpr unsigned long meter_transport_status_ioctl =
	_IOR('W', 0x02, MeterTransportStatusIoctl);

} // namespace

MeterDmaReader::MeterDmaReader(std::string device_path)
	: device_path_(std::move(device_path))
{
}

MeterDmaReader::~MeterDmaReader()
{
	stop();
}

void MeterDmaReader::start()
{
	if (fd_ >= 0)
		return;
	fd_ = ::open(device_path_.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd_ < 0)
		throw_errno("open " + device_path_);
}

void MeterDmaReader::stop() noexcept
{
	if (fd_ >= 0)
		(void)::close(fd_);
	fd_ = -1;
}

MeterRecordBatch MeterDmaReader::read_available()
{
	if (fd_ < 0)
		throw std::logic_error("meter DMA reader is not started");

	MeterRecordBatch result{};
	const auto size = ::read(fd_, result.records.data(),
				 sizeof(result.records));
	if (size < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
			return result;
		throw_errno("read " + device_path_);
	}
	result.bytes = static_cast<std::size_t>(size);
	result.partial_record =
		result.bytes % sizeof(MeterRecord) != 0;
	if (!result.partial_record)
		result.count = result.bytes / sizeof(MeterRecord);
	return result;
}

std::uint64_t MeterDmaReader::transport_overruns() noexcept
{
	if (fd_ < 0)
		return 0;
	MeterTransportStatusIoctl status{};
	if (::ioctl(fd_, meter_transport_status_ioctl, &status) != 0)
		return 0;
	return status.overrun_blocks;
}

} // namespace msap1::acquisition
