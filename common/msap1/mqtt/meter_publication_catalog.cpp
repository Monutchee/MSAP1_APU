#include "msap1/mqtt/meter_publication_catalog.hpp"

#include "mnc/MeterDataProvider/attributes/meter_attribute.hpp"

#include <stdexcept>

namespace msap1::mqtt {
namespace {

std::string unit_id(mnc::meter::MeterUnit unit)
{
	switch (unit) {
	case mnc::meter::MeterUnit::MilliHertz: return "Hz";
	case mnc::meter::MeterUnit::MicroVolts: return "V";
	case mnc::meter::MeterUnit::MicroAmperes: return "A";
	case mnc::meter::MeterUnit::Picowatts: return "W";
	case mnc::meter::MeterUnit::PicoVoltAmperes: return "VA";
	case mnc::meter::MeterUnit::PowerFactorMillionths: return "PF";
	case mnc::meter::MeterUnit::Picovars: return "var";
	case mnc::meter::MeterUnit::Millidegrees: return "deg";
	case mnc::meter::MeterUnit::RatioMillionths: return "%";
	}
	return "unknown";
}

} // namespace

std::vector<PublicationPeriodCapability> MeterPublicationCatalog::capabilities(
	const mnc::meter::MeterSnapshotProvider &provider)
{
	std::vector<PublicationPeriodCapability> result;
	for (const auto &period : provider.capabilities()) {
		PublicationPeriodCapability output{.id = period_id(period.period),
			.attributes = {}};
		for (const auto attribute : period.attributes) {
			const auto descriptor = mnc::meter::describe(attribute);
			output.attributes.push_back({std::string(descriptor.key),
				unit_id(descriptor.unit)});
		}
		result.push_back(std::move(output));
	}
	return result;
}

mnc::meter::MeasurementPeriod MeterPublicationCatalog::period(std::string_view id)
{
	using Period = mnc::meter::MeasurementPeriod;
	if (id == "basic") return Period::Basic;
	if (id == "cycles150_180") return Period::Cycles150_180;
	if (id == "min10") return Period::Min10;
	if (id == "hour2") return Period::Hour2;
	if (id == "min10_live") return Period::Min10Live;
	if (id == "hour2_live") return Period::Hour2Live;
	throw std::invalid_argument("unknown meter publication period");
}

std::string MeterPublicationCatalog::period_id(
	mnc::meter::MeasurementPeriod period)
{
	switch (period) {
	case mnc::meter::MeasurementPeriod::Basic: return "basic";
	case mnc::meter::MeasurementPeriod::Cycles150_180:
		return "cycles150_180";
	case mnc::meter::MeasurementPeriod::Min10: return "min10";
	case mnc::meter::MeasurementPeriod::Hour2: return "hour2";
	case mnc::meter::MeasurementPeriod::Min10Live: return "min10_live";
	case mnc::meter::MeasurementPeriod::Hour2Live: return "hour2_live";
	}
	return "unknown";
}

} // namespace msap1::mqtt
