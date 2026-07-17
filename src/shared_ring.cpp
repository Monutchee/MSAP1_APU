#include "msap1/shared_ring.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace msap1 {
namespace {

[[noreturn]] void throw_errno(const std::string &operation)
{
	throw std::runtime_error(operation + ": " + std::strerror(errno));
}

std::size_t mapping_size(std::uint32_t capacity)
{
	return sizeof(SharedRingHeader) +
		static_cast<std::size_t>(capacity) * sizeof(AdcSampleFrame);
}

} // namespace

SharedRingWriter::SharedRingWriter(std::string name, std::uint32_t capacity)
{
	if (capacity == 0)
		throw std::invalid_argument("shared ring capacity cannot be zero");
	fd_ = ::shm_open(name.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0660);
	if (fd_ < 0)
		throw_errno("shm_open " + name);
	mapping_size_ = mapping_size(capacity);
	if (::ftruncate(fd_, static_cast<off_t>(mapping_size_)) < 0)
		throw_errno("resize shared ADC ring");
	(void)::fchmod(fd_, 0660);
	mapping_ = ::mmap(nullptr, mapping_size_, PROT_READ | PROT_WRITE,
			  MAP_SHARED, fd_, 0);
	if (mapping_ == MAP_FAILED) {
		mapping_ = nullptr;
		throw_errno("map shared ADC ring");
	}

	std::memset(mapping_, 0, mapping_size_);
	header_ = static_cast<SharedRingHeader *>(mapping_);
	frames_ = reinterpret_cast<AdcSampleFrame *>(
		static_cast<unsigned char *>(mapping_) + sizeof(SharedRingHeader));
	header_->magic = shared_ring_magic;
	header_->version = shared_ring_version;
	header_->header_size = sizeof(SharedRingHeader);
	header_->capacity = capacity;
	header_->frame_size = sizeof(AdcSampleFrame);
	header_->channel_count = adc_channel_count;
	header_->sample_rate_hz = adc_default_sample_rate_hz;
	header_->generation = 1;
	__atomic_store_n(&header_->published_sequence, 0, __ATOMIC_RELEASE);
}

SharedRingWriter::~SharedRingWriter()
{
	if (mapping_ != nullptr)
		::munmap(mapping_, mapping_size_);
	if (fd_ >= 0)
		::close(fd_);
}

void SharedRingWriter::publish(const AdcSampleFrame *frames, std::size_t count)
{
	if (frames == nullptr || count == 0)
		return;
	auto sequence = __atomic_load_n(&header_->published_sequence,
					__ATOMIC_RELAXED);
	for (std::size_t index = 0; index < count; ++index)
		frames_[(sequence + index) % header_->capacity] = frames[index];
	sequence += count;
	header_->frames_received = sequence;
	__atomic_store_n(&header_->published_sequence, sequence, __ATOMIC_RELEASE);
}

void SharedRingWriter::set_running(bool running)
{
	__atomic_store_n(&header_->running, running ? 1u : 0u, __ATOMIC_RELEASE);
}

void SharedRingWriter::set_capture_flags(std::uint32_t flags)
{
	__atomic_store_n(&header_->capture_flags, flags, __ATOMIC_RELEASE);
}

void SharedRingWriter::note_read_error()
{
	__atomic_add_fetch(&header_->iio_read_errors, 1u, __ATOMIC_RELAXED);
}

SharedRingReader::SharedRingReader(std::string name)
{
	fd_ = ::shm_open(name.c_str(), O_RDONLY | O_CLOEXEC, 0);
	if (fd_ < 0)
		throw_errno("shm_open " + name);
	struct stat info {};
	if (::fstat(fd_, &info) < 0)
		throw_errno("stat shared ADC ring");
	mapping_size_ = static_cast<std::size_t>(info.st_size);
	if (mapping_size_ < sizeof(SharedRingHeader))
		throw std::runtime_error("shared ADC ring is truncated");
	mapping_ = ::mmap(nullptr, mapping_size_, PROT_READ, MAP_SHARED, fd_, 0);
	if (mapping_ == MAP_FAILED) {
		mapping_ = nullptr;
		throw_errno("map shared ADC ring");
	}
	header_ = static_cast<const SharedRingHeader *>(mapping_);
	if (header_->magic != shared_ring_magic ||
	    header_->version != shared_ring_version ||
	    header_->header_size != sizeof(SharedRingHeader) ||
	    header_->frame_size != sizeof(AdcSampleFrame) ||
	    header_->channel_count != adc_channel_count ||
	    header_->capacity == 0 ||
	    mapping_size_ < mapping_size(header_->capacity))
		throw std::runtime_error("shared ADC ring layout is incompatible");
	frames_ = reinterpret_cast<const AdcSampleFrame *>(
		static_cast<const unsigned char *>(mapping_) +
		sizeof(SharedRingHeader));
}

SharedRingReader::~SharedRingReader()
{
	if (mapping_ != nullptr)
		::munmap(mapping_, mapping_size_);
	if (fd_ >= 0)
		::close(fd_);
}

std::uint64_t SharedRingReader::published_sequence() const
{
	return __atomic_load_n(&header_->published_sequence, __ATOMIC_ACQUIRE);
}

std::uint32_t SharedRingReader::sample_rate_hz() const
{
	return header_->sample_rate_hz;
}

std::uint32_t SharedRingReader::capture_flags() const
{
	return __atomic_load_n(&header_->capture_flags, __ATOMIC_ACQUIRE);
}

bool SharedRingReader::running() const
{
	return __atomic_load_n(&header_->running, __ATOMIC_ACQUIRE) != 0u;
}

bool SharedRingReader::read(std::uint64_t &cursor, AdcSampleFrame &frame,
			    std::uint64_t &dropped) const
{
	for (;;) {
		const auto published = published_sequence();
		const auto oldest = published > header_->capacity ?
			published - header_->capacity : 0u;
		if (cursor < oldest) {
			dropped += oldest - cursor;
			cursor = oldest;
		}
		if (cursor >= published)
			return false;

		const auto sequence = cursor;
		frame = frames_[sequence % header_->capacity];
		const auto after = published_sequence();
		if (after - sequence > header_->capacity)
			continue;
		cursor = sequence + 1;
		return true;
	}
}

} // namespace msap1
