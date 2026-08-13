#pragma once

#include "mnc/MeterDataProvider/snapshot/meter_snapshot_provider.hpp"

#include <string>
#include <vector>

namespace msap1::mqtt {

struct PublicationAttributeCapability {
	std::string id;
	std::string unit;
};

struct PublicationPeriodCapability {
	std::string id;
	std::vector<PublicationAttributeCapability> attributes;
};

class MeterPublicationCatalog {
public:
	[[nodiscard]] static std::vector<PublicationPeriodCapability>
	capabilities(const mnc::meter::MeterSnapshotProvider &provider);

	[[nodiscard]] static mnc::meter::MeasurementPeriod
	period(std::string_view id);
	[[nodiscard]] static std::string period_id(
		mnc::meter::MeasurementPeriod period);
};

} // namespace msap1::mqtt
