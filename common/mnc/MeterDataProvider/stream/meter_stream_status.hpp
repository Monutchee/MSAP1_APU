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
	/** First cursor this spool session can issue.  Strictly increases across
	 * spool restarts, so a consumer can distinguish "nothing was ever
	 * published before my oldest record" from "records existed and are
	 * gone" — the difference a volatile spool erases. */
	std::uint64_t session_start_cursor = 0;
	/** Records the hard byte cap evicted before any consumer acknowledged
	 * them; nonzero means bounded, reported loss under a wedged consumer. */
	std::uint64_t dropped_unacknowledged_records = 0;

	struct ConsumerCursor {
		std::string name;
		std::uint64_t acknowledged_cursor = 0;
	};

	std::vector<ConsumerCursor> consumers;
};

} // namespace mnc::meter_stream
