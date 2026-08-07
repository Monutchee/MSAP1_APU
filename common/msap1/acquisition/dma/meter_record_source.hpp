#ifndef MSAP1_ACQUISITION_METER_RECORD_SOURCE_HPP
#define MSAP1_ACQUISITION_METER_RECORD_SOURCE_HPP

#include "msap1/meter/meter_record.hpp"

#include <array>
#include <cstddef>
#include <string_view>

namespace msap1::acquisition {

/** Result of one nonblocking read from a meter-record source. */
struct MeterRecordBatch {
	static constexpr std::size_t capacity = 16;
	std::array<MeterRecord, capacity> records{};
	std::size_t count = 0;
	std::size_t bytes = 0;
	bool partial_record = false;
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
};

} // namespace msap1::acquisition

#endif
