#include "msap1/acquisition/meter_dma_reader.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

namespace msap1::acquisition {
namespace {

[[noreturn]] void throw_errno(const std::string &operation)
{
	throw std::runtime_error(operation + ": " + std::strerror(errno));
}

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

} // namespace msap1::acquisition
