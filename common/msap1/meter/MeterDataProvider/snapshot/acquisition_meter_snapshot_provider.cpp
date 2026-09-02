#include "msap1/meter/MeterDataProvider/snapshot/acquisition_meter_snapshot_provider.hpp"

#include "mnc/MeterDataProvider/attributes/meter_attribute_set.hpp"

#include <stdexcept>

namespace msap1::meter {
namespace {

std::vector<mnc::meter::MeterAttributeKey> supported(
	mnc::meter::MeasurementPeriod period)
{
	using Id = mnc::meter::MeterAttributeId;
	if (period == mnc::meter::MeasurementPeriod::Seconds10)
		return {{Id::Frequency, std::nullopt}};
	if (period == mnc::meter::MeasurementPeriod::Demand) {
		std::vector<mnc::meter::MeterAttributeKey> result;
		for (const auto attribute :
		     mnc::meter::attributes_in(mnc::meter::MeterAttributeGroup::Demand))
			result.push_back(attribute);
		return result;
	}
	std::vector<mnc::meter::MeterAttributeKey> result{
		{Id::VanRms, std::nullopt}, {Id::VbnRms, std::nullopt},
		{Id::VcnRms, std::nullopt}, {Id::IaRms, std::nullopt},
		{Id::IbRms, std::nullopt}, {Id::IcRms, std::nullopt},
		{Id::InRms, std::nullopt},
	};
	if (period == mnc::meter::MeasurementPeriod::Basic)
		result.insert(result.begin(), {Id::Frequency, std::nullopt});
	for (const auto id : {Id::VabRms, Id::VbcRms, Id::VcaRms,
			      Id::ActivePowerA, Id::ActivePowerB,
			      Id::ActivePowerC, Id::ActivePowerTotal,
			      Id::ApparentPowerA, Id::ApparentPowerB,
			      Id::ApparentPowerC, Id::ApparentPowerTotal,
			      Id::PowerFactorA, Id::PowerFactorB,
			      Id::PowerFactorC, Id::PowerFactorTotal,
			      Id::ReactivePowerA, Id::ReactivePowerB,
			      Id::ReactivePowerC, Id::ReactivePowerTotal,
			      Id::DisplacementPowerFactorA,
			      Id::DisplacementPowerFactorB,
			      Id::DisplacementPowerFactorC,
			      Id::DisplacementPowerFactorTotal,
			      Id::VoltagePhaseAngleA, Id::VoltagePhaseAngleB,
			      Id::VoltagePhaseAngleC, Id::CurrentPhaseAngleA,
			      Id::CurrentPhaseAngleB, Id::CurrentPhaseAngleC,
			      Id::VoltageUnbalance, Id::CurrentUnbalance,
			      Id::VoltageZeroSequenceRatio,
			      Id::CurrentZeroSequenceRatio,
			      Id::ZeroSequenceVoltage,
			      Id::PositiveSequenceVoltage,
			      Id::NegativeSequenceVoltage,
			      Id::ZeroSequenceCurrent,
			      Id::PositiveSequenceCurrent,
			      Id::NegativeSequenceCurrent})
		result.push_back({id, std::nullopt});
	if (period == mnc::meter::MeasurementPeriod::Basic)
		for (const auto attribute : mnc::meter::attributes_in(
			mnc::meter::MeterAttributeGroup::Energy))
			result.push_back(attribute);
	return result;
}

} // namespace

std::vector<mnc::meter::MeterCapabilities>
AcquisitionMeterSnapshotProvider::capabilities() const
{
	using Period = mnc::meter::MeasurementPeriod;
	return {
		{Period::Basic, supported(Period::Basic)},
		{Period::Seconds10, supported(Period::Seconds10)},
		{Period::Cycles150_180, supported(Period::Cycles150_180)},
		{Period::Min10, supported(Period::Min10)},
		{Period::Hour2, supported(Period::Hour2)},
		{Period::Min10Live, supported(Period::Min10Live)},
		{Period::Hour2Live, supported(Period::Hour2Live)},
		{Period::Demand, supported(Period::Demand)},
	};
}

std::optional<mnc::meter::MeterSnapshot>
AcquisitionMeterSnapshotProvider::latest(
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
AcquisitionMeterSnapshotProvider::subscribe_latest(
	const mnc::meter::MeterSnapshotRequest &, Callback)
{
	throw std::logic_error(
		"acquisition IPC does not expose latest-state subscriptions");
}

} // namespace msap1::meter
