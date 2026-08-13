#pragma once

#include "mnc/MeterDataProvider/snapshot/meter_snapshot_provider.hpp"
#include "msap1/acquisition/ipc/acquisition_ipc.hpp"

#include <mutex>
#include <utility>

namespace msap1::meter {

/**
 * Cross-process MeterSnapshotProvider backed by the acquisition daemon.
 *
 * Long-running protocol services use this adapter instead of learning the
 * acquisition IPC vocabulary. Each latest() call is one coherent typed
 * snapshot request. The current IPC exposes one-shot snapshots only, so
 * subscribe_latest() is intentionally unsupported.
 */
class AcquisitionMeterDataProvider final
	: public mnc::meter::MeterSnapshotProvider {
public:
	explicit AcquisitionMeterDataProvider(
		std::string socket_path = acquisition_socket_path)
		: client_(std::move(socket_path))
	{
	}

	[[nodiscard]] std::vector<mnc::meter::MeterCapabilities>
	capabilities() const override;
	[[nodiscard]] std::optional<mnc::meter::MeterSnapshot>
	latest(const mnc::meter::MeterSnapshotRequest &request) const override;
	[[nodiscard]] mnc::meter::LatestSubscription subscribe_latest(
		const mnc::meter::MeterSnapshotRequest &request,
		Callback callback) override;

private:
	/* AcquisitionClient multiplexes correlated requests, but serializing this
	 * facade also bounds the number of blocked Modbus client reads. */
	mutable std::mutex mutex_;
	mutable msap1::AcquisitionClient client_;
};

} // namespace msap1::meter
