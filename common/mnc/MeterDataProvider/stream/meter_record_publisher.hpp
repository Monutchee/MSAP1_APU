#pragma once

#include "mnc/MeterDataProvider/stream/meter_stream_record.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace mnc::meter_stream {

/**
 * Write-side port for durably accepting producer records.
 *
 * This interface is intentionally not part of the consumer-facing
 * MeterDataProvider facade. Acquisition receives this narrow capability and
 * must publish a validated record before updating the lossy snapshot path.
 */
class MeterRecordPublisher {
public:
	virtual ~MeterRecordPublisher() = default;

	/** Returns the committed ordered cursor. Retries are idempotent. */
	virtual std::uint64_t publish(const MeterStreamRecord &record) = 0;

	/**
	 * Atomically commit an ordered record family.
	 *
	 * The returned cursor at each index belongs to the input at that index.
	 * Either every record is committed or none is; retries are idempotent.
	 */
	virtual std::vector<std::uint64_t> publish_records(
		std::span<const MeterStreamRecord> records) = 0;
};

} // namespace mnc::meter_stream
