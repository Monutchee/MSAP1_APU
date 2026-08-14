#pragma once

#include "mnc/MeterDataProvider/snapshot/meter_snapshot.hpp"

#include <span>
#include <string>

namespace msap1::mqtt {

class MeterSnapshotPayloadEncoder {
public:
	[[nodiscard]] std::string encode(
		const mnc::meter::MeterSnapshot &snapshot,
		std::string_view publication_id,
		std::span<const mnc::meter::MeterAttributeKey> selected) const;
};

} // namespace msap1::mqtt
