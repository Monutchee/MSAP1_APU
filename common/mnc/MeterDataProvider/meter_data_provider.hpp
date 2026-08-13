#pragma once

#include "mnc/MeterDataProvider/snapshot/meter_snapshot_provider.hpp"
#include "mnc/MeterDataProvider/stream/meter_stream_consumer.hpp"

namespace mnc::meter {

/**
 * Consumer-facing facade for the two supported meter-data access models.
 *
 * snapshot_provider() is the latest typed state and may skip intermediate
 * updates. stream_consumer() is ordered cursor-based delivery and does not
 * skip accepted records. The producer-side MeterRecordPublisher deliberately
 * remains a separate interface so consumers cannot inject records.
 */
class MeterDataProvider {
public:
	virtual ~MeterDataProvider() = default;

	[[nodiscard]] virtual MeterSnapshotProvider &
	snapshot_provider() noexcept = 0;
	[[nodiscard]] virtual const MeterSnapshotProvider &
	snapshot_provider() const noexcept = 0;
	[[nodiscard]] virtual mnc::meter_stream::MeterStreamConsumer &
	stream_consumer() noexcept = 0;
};

/**
 * Non-owning dependency-injection adapter for an independently implemented
 * snapshot provider and durable stream consumer.
 *
 * Both referenced objects must outlive this facade. Applications should pass
 * only the narrow snapshot_provider() or stream_consumer() result to
 * subsystems that do not need both capabilities.
 */
class MeterDataProviderView final : public MeterDataProvider {
public:
	MeterDataProviderView(
		MeterSnapshotProvider &snapshot_provider,
		mnc::meter_stream::MeterStreamConsumer &stream_consumer) noexcept
		: snapshot_provider_(snapshot_provider),
		  stream_consumer_(stream_consumer)
	{
	}

	[[nodiscard]] MeterSnapshotProvider &
	snapshot_provider() noexcept override
	{
		return snapshot_provider_;
	}

	[[nodiscard]] const MeterSnapshotProvider &
	snapshot_provider() const noexcept override
	{
		return snapshot_provider_;
	}

	[[nodiscard]] mnc::meter_stream::MeterStreamConsumer &
	stream_consumer() noexcept override
	{
		return stream_consumer_;
	}

private:
	MeterSnapshotProvider &snapshot_provider_;
	mnc::meter_stream::MeterStreamConsumer &stream_consumer_;
};

} // namespace mnc::meter
