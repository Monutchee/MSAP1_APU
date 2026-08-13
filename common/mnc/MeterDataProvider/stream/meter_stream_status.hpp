#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mnc::meter_stream {

/** Current bounds and independent durable-consumer positions. */
struct StreamStatus {
	bool durability = true;
	std::uint64_t oldest_cursor = 0;
	std::uint64_t newest_cursor = 0;
	std::uint64_t record_count = 0;
	std::uint64_t storage_bytes = 0;

	struct ConsumerCursor {
		std::string name;
		std::uint64_t acknowledged_cursor = 0;
	};

	std::vector<ConsumerCursor> consumers;
};

} // namespace mnc::meter_stream
