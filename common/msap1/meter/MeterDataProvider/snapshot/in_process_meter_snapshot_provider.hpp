#pragma once

#include "mnc/MeterDataProvider/snapshot/meter_snapshot_provider.hpp"
#include "msap1/meter/meter_data.hpp"

namespace msap1::meter {

/** Product adapter that projects decoded MSAP1 values onto the reusable API. */
class InProcessMeterSnapshotProvider final
	: public mnc::meter::MeterSnapshotProvider {
public:
	explicit InProcessMeterSnapshotProvider(msap1::MeterData &data)
		: data_(data)
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
	[[nodiscard]] static mnc::meter::MeterSnapshot project(
		const msap1::MeterPeriodView &view,
		const mnc::meter::MeterSnapshotRequest &request);

	msap1::MeterData &data_;
};

} // namespace msap1::meter
