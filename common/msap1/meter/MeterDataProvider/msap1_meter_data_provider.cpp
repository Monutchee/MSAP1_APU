#include "msap1/meter/MeterDataProvider/msap1_meter_data_provider.hpp"

#include <chrono>
#include <stdexcept>

namespace msap1::meter {

namespace {

using mnc::meter::MeterAttributeId;
using mnc::meter::MeterAttributeKey;
using mnc::meter::MeterAttributeSet;
using mnc::meter::MeterAttributeValue;
using mnc::meter::MeterUnit;
using mnc::meter::ReadingQuality;

mnc::meter::TimeQuality time_quality(msap1::TimeQuality value)
{
	switch (value) {
	case msap1::TimeQuality::Synchronized:
		return mnc::meter::TimeQuality::Synchronized;
	case msap1::TimeQuality::Holdover:
		return mnc::meter::TimeQuality::Holdover;
	case msap1::TimeQuality::Unsynchronized:
		break;
	}
	return mnc::meter::TimeQuality::Unsynchronized;
}

template<typename Timing>
std::optional<mnc::meter::MeterSnapshotTiming> snapshot_timing(
	const Timing &timing)
{
	mnc::meter::MeterSnapshotTiming result{};
	result.quality = time_quality(timing.time_quality);
	result.first_sample_index = timing.first_sample_index;
	result.sample_count = timing.sample_count;
	result.cycle_count = timing.cycle_count;
	result.nominal_frequency_hz =
		static_cast<std::uint32_t>(timing.nominal_frequency);
	if (timing.utc_start) {
		result.utc_start_nanoseconds =
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				timing.utc_start->time_since_epoch()).count();
		result.utc_uncertainty_nanoseconds = timing.utc_uncertainty_ns;
	}
	return result;
}

ReadingQuality quality(msap1::MeasurementQuality value)
{
	switch (value) {
	case msap1::MeasurementQuality::unavailable:
		return ReadingQuality::Unavailable;
	case msap1::MeasurementQuality::valid:
		return ReadingQuality::Valid;
	case msap1::MeasurementQuality::invalid:
		return ReadingQuality::Invalid;
	case msap1::MeasurementQuality::out_of_range:
		return ReadingQuality::OutOfRange;
	case msap1::MeasurementQuality::timed_out:
		return ReadingQuality::TimedOut;
	case msap1::MeasurementQuality::arithmetic_error:
		return ReadingQuality::ArithmeticError;
	}
	return ReadingQuality::Unavailable;
}

template<typename Unit>
MeterAttributeValue value(MeterAttributeKey attribute, MeterUnit unit,
			  const msap1::Reading<Unit> &reading)
{
	return {
		attribute,
		unit,
		quality(reading.quality),
		reading.value,
		reading.source_sequence,
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			reading.measured_at.time_since_epoch()).count(),
		reading.calculation_window.sample_count,
		reading.calculation_window.duration.count(),
	};
}

MeterAttributeValue unavailable(MeterAttributeKey attribute)
{
	const auto descriptor = mnc::meter::describe(attribute);
	return {attribute, descriptor.unit, ReadingQuality::Unavailable};
}

std::vector<MeterAttributeKey> supported(msap1::MeasurementPeriod period)
{
	using Id = MeterAttributeId;
	if (period == msap1::MeasurementPeriod::Min10 ||
	    period == msap1::MeasurementPeriod::Hour2)
		return {};
	std::vector<MeterAttributeKey> result{
		{Id::VanRms, std::nullopt}, {Id::VbnRms, std::nullopt},
		{Id::VcnRms, std::nullopt}, {Id::IaRms, std::nullopt},
		{Id::IbRms, std::nullopt}, {Id::IcRms, std::nullopt},
		{Id::InRms, std::nullopt},
	};
	if (period == msap1::MeasurementPeriod::Basic)
		result.insert(result.begin(), MeterAttributeKey{Id::Frequency,
							       std::nullopt});
	return result;
}

} // namespace

std::vector<mnc::meter::MeterCapabilities>
Msap1MeterDataProvider::capabilities() const
{
	return {
		{MeasurementPeriod::Basic, supported(MeasurementPeriod::Basic)},
		{MeasurementPeriod::Cycles150_180,
		 supported(MeasurementPeriod::Cycles150_180)},
		{MeasurementPeriod::Min10, {}},
		{MeasurementPeriod::Hour2, {}},
	};
}

mnc::meter::MeterSnapshot Msap1MeterDataProvider::project(
	const msap1::MeterPeriodView &view,
	const mnc::meter::MeterSnapshotRequest &request)
{
	MeterAttributeSet selection(request.attributes);
	if (selection.empty())
		selection = MeterAttributeSet(supported(view.period));

	mnc::meter::MeterSnapshot result{};
	result.period = view.period;
	result.sequence = view.latest_sequence;
	result.configuration_generation = view.configuration_generation;
	result.updated_at_nanoseconds =
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			view.updated_at.time_since_epoch()).count();
	/* Timing is copied from the immutable view populated by the record
	 * ingestor. It is measurement provenance, never reconstructed from the
	 * current clock at request time. */
	if (view.timing)
		result.timing = snapshot_timing(*view.timing);
	else if (view.aggregate_timing)
		result.timing = snapshot_timing(*view.aggregate_timing);

	for (const auto attribute : selection.values()) {
		/* Indexed attributes are reserved for future families such as
		 * harmonic order.  The current PL records provide fundamentals
		 * only, so an indexed key must not alias the unindexed reading. */
		if (attribute.index) {
			result.values.push_back(unavailable(attribute));
			continue;
		}
		switch (attribute.id) {
		case MeterAttributeId::Frequency:
			result.values.push_back(value(attribute, MeterUnit::MilliHertz,
				view.values.fundamental.frequency));
			break;
		case MeterAttributeId::VanRms:
			result.values.push_back(value(attribute, MeterUnit::MicroVolts,
				view.values.fundamental.voltage_ln.phase_a));
			break;
		case MeterAttributeId::VbnRms:
			result.values.push_back(value(attribute, MeterUnit::MicroVolts,
				view.values.fundamental.voltage_ln.phase_b));
			break;
		case MeterAttributeId::VcnRms:
			result.values.push_back(value(attribute, MeterUnit::MicroVolts,
				view.values.fundamental.voltage_ln.phase_c));
			break;
		case MeterAttributeId::IaRms:
			result.values.push_back(value(attribute, MeterUnit::MicroAmperes,
				view.values.fundamental.current.phase_a));
			break;
		case MeterAttributeId::IbRms:
			result.values.push_back(value(attribute, MeterUnit::MicroAmperes,
				view.values.fundamental.current.phase_b));
			break;
		case MeterAttributeId::IcRms:
			result.values.push_back(value(attribute, MeterUnit::MicroAmperes,
				view.values.fundamental.current.phase_c));
			break;
		case MeterAttributeId::InRms:
			result.values.push_back(value(attribute, MeterUnit::MicroAmperes,
				view.values.fundamental.current.neutral));
			break;
		case MeterAttributeId::VabRms:
		case MeterAttributeId::VbcRms:
		case MeterAttributeId::VcaRms:
			result.values.push_back(unavailable(attribute));
			break;
		}
	}
	return result;
}

std::optional<mnc::meter::MeterSnapshot> Msap1MeterDataProvider::latest(
	const mnc::meter::MeterSnapshotRequest &request) const
{
	const auto view = data_.latest(request.period);
	if (!view)
		return std::nullopt;
	return project(*view, request);
}

mnc::meter::LatestSubscription Msap1MeterDataProvider::subscribe_latest(
	const mnc::meter::MeterSnapshotRequest &request, Callback callback)
{
	if (!callback)
		throw std::invalid_argument("meter snapshot callback is empty");
	struct Owner {
		msap1::MeterData::Subscription subscription;
	};
	auto owner = std::make_shared<Owner>(Owner{data_.subscribe(
		request.period,
		[request, callback = std::move(callback)](
			const msap1::MeterPeriodView &view) {
			callback(project(view, request));
		})});
	return mnc::meter::LatestSubscription{std::move(owner)};
}

} // namespace msap1::meter
