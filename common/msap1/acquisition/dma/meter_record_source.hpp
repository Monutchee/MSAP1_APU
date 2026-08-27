#ifndef MSAP1_ACQUISITION_METER_RECORD_SOURCE_HPP
#define MSAP1_ACQUISITION_METER_RECORD_SOURCE_HPP

#include "msap1/meter/meter_record.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace msap1::acquisition {

/** Result of one nonblocking read from a meter-record source. */
struct MeterRecordBatch {
	static constexpr std::size_t capacity = 128;
	std::array<MeterRecord, capacity> records{};
	std::size_t count = 0;
	std::size_t bytes = 0;
	bool partial_record = false;
};

/**
 * Kernel transport accounting for one meter-record source, absolute since
 * start().
 *
 * Read as a whole so every counter describes the same instant: comparing a
 * produced total against a consumed total sampled a moment later would invent
 * a backlog that never existed.  Sources without transport accounting report
 * this zeroed.
 */
struct MeterTransportStatus {
	std::uint64_t produced_blocks = 0;
	std::uint64_t consumed_blocks = 0;
	std::uint64_t overrun_blocks = 0;
	/* Cyclic completion callbacks the driver saw.  Widened from the
	 * kernel's 32-bit counter so arithmetic against produced_blocks stays
	 * in one type. */
	std::uint64_t callbacks = 0;
	std::uint32_t ring_blocks = 0;
};

/**
 * Abstract source of complete PL meter records.
 *
 * The acquisition coordinator owns the lifecycle and polling order. Test
 * doubles can implement this interface without opening a Linux device.
 */
class MeterRecordSource {
public:
	virtual ~MeterRecordSource() = default;

	virtual void start() = 0;
	virtual void stop() noexcept = 0;
	[[nodiscard]] virtual int native_handle() const noexcept = 0;
	[[nodiscard]] virtual std::string_view name() const noexcept = 0;
	[[nodiscard]] virtual MeterRecordBatch read_available() = 0;
	/**
	 * Whole kernel transport accounting since start(), absolute.  Zeroed
	 * for sources without transport accounting.  Observability only: no
	 * transport decision reads this.
	 */
	[[nodiscard]] virtual MeterTransportStatus transport_status() noexcept
	{
		return {};
	}
	/**
	 * Records lost in the kernel transport ring since start(), absolute.
	 * Zero for sources without transport accounting.  A payload sequence gap
	 * with no matching overrun growth is PL-side loss; with matching growth
	 * it is kernel-side (userspace fell behind the DMA ring) — the
	 * distinction that decides where to look next.
	 */
	[[nodiscard]] virtual std::uint64_t transport_overruns() noexcept
	{
		return transport_status().overrun_blocks;
	}
};

} // namespace msap1::acquisition

#endif
