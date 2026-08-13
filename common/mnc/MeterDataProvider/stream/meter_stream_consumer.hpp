#pragma once

#include "mnc/MeterDataProvider/stream/meter_stream_record.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace mnc::meter_stream {

/** Ordered, cursor-based read access that does not skip accepted records. */
class MeterStreamConsumer {
public:
	virtual ~MeterStreamConsumer() = default;

	virtual void register_consumer(std::string_view name) = 0;
	virtual void unregister_consumer(std::string_view name) = 0;
	virtual std::vector<MeterStreamRecord> read_after(
		std::string_view name, std::size_t limit) = 0;
	virtual void acknowledge(std::string_view name,
		std::uint64_t cursor) = 0;
};

} // namespace mnc::meter_stream
