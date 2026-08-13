#pragma once

#include "mnc/MeterDataProvider/stream/meter_stream_record.hpp"

#include <cstdint>

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
};

} // namespace mnc::meter_stream
