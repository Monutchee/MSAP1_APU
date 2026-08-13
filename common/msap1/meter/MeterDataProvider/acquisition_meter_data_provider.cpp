#include "msap1/meter/MeterDataProvider/acquisition_meter_data_provider.hpp"

#include <stdexcept>

namespace msap1::meter {
namespace {

std::vector<mnc::meter::MeterAttributeKey> supported(
	mnc::meter::MeasurementPeriod period)
{
	using Id = mnc::meter::MeterAttributeId;
	std::vector<mnc::meter::MeterAttributeKey> result{
		{Id::VanRms, std::nullopt}, {Id::VbnRms, std::nullopt},
		{Id::VcnRms, std::nullopt}, {Id::IaRms, std::nullopt},
		{Id::IbRms, std::nullopt}, {Id::IcRms, std::nullopt},
		{Id::InRms, std::nullopt},
	};
	if (period == mnc::meter::MeasurementPeriod::Basic)
		result.insert(result.begin(), {Id::Frequency, std::nullopt});
	return result;
}

} // namespace

std::vector<mnc::meter::MeterCapabilities>
AcquisitionMeterDataProvider::capabilities() const
{
	using Period = mnc::meter::MeasurementPeriod;
	return {
		{Period::Basic, supported(Period::Basic)},
		{Period::Cycles150_180, supported(Period::Cycles150_180)},
	};
}

std::optional<mnc::meter::MeterSnapshot>
AcquisitionMeterDataProvider::latest(
	const mnc::meter::MeterSnapshotRequest &request) const
{
	std::scoped_lock lock(mutex_);
	msap1::MeterSnapshotRequest wire;
	wire.selection = request;
	const auto response = client_.request(wire);
	if (response.status != msap1::AcquisitionStatus::ok)
		throw msap1::AcquisitionUnavailable(
			"acquisition rejected meter snapshot request");
	if (!response.running || !response.has_snapshot)
		return std::nullopt;
	return response.snapshot;
}

mnc::meter::LatestSubscription
AcquisitionMeterDataProvider::subscribe_latest(
	const mnc::meter::MeterSnapshotRequest &, Callback)
{
	throw std::logic_error(
		"acquisition IPC does not expose latest-state subscriptions");
}

} // namespace msap1::meter
