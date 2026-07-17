#ifndef MSAP1_SHARED_RING_HPP
#define MSAP1_SHARED_RING_HPP

#include "msap1/acquisition_ipc.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace msap1 {

inline constexpr std::uint32_t shared_ring_magic = 0x4d534152u;
inline constexpr std::uint16_t shared_ring_version = 1;

struct alignas(64) SharedRingHeader {
	std::uint32_t magic;
	std::uint16_t version;
	std::uint16_t header_size;
	std::uint32_t capacity;
	std::uint32_t frame_size;
	std::uint32_t channel_count;
	std::uint32_t sample_rate_hz;
	std::uint32_t running;
	std::uint32_t capture_flags;
	alignas(8) std::uint64_t published_sequence;
	std::uint64_t generation;
	std::uint64_t frames_received;
	std::uint64_t iio_read_errors;
	std::uint64_t reserved[7];
};

class SharedRingWriter {
public:
	explicit SharedRingWriter(
		std::string name = acquisition_shm_name,
		std::uint32_t capacity = acquisition_ring_capacity);
	~SharedRingWriter();

	SharedRingWriter(const SharedRingWriter &) = delete;
	SharedRingWriter &operator=(const SharedRingWriter &) = delete;

	void publish(const AdcSampleFrame *frames, std::size_t count);
	void set_running(bool running);
	void set_capture_flags(std::uint32_t flags);
	void note_read_error();
	const SharedRingHeader &header() const { return *header_; }

private:
	int fd_ = -1;
	void *mapping_ = nullptr;
	std::size_t mapping_size_ = 0;
	SharedRingHeader *header_ = nullptr;
	AdcSampleFrame *frames_ = nullptr;
};

class SharedRingReader {
public:
	explicit SharedRingReader(std::string name = acquisition_shm_name);
	~SharedRingReader();

	SharedRingReader(const SharedRingReader &) = delete;
	SharedRingReader &operator=(const SharedRingReader &) = delete;

	std::uint64_t published_sequence() const;
	std::uint32_t sample_rate_hz() const;
	std::uint32_t capture_flags() const;
	bool running() const;

	// cursor is the independent next sequence for this reader. If it falls
	// behind the ring, it is advanced and dropped receives the skipped count.
	bool read(std::uint64_t &cursor, AdcSampleFrame &frame,
		  std::uint64_t &dropped) const;

private:
	int fd_ = -1;
	void *mapping_ = nullptr;
	std::size_t mapping_size_ = 0;
	const SharedRingHeader *header_ = nullptr;
	const AdcSampleFrame *frames_ = nullptr;
};

} // namespace msap1

#endif
