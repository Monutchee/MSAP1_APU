#pragma once

#include "mnc/MeterDataProvider/attributes/meter_attribute.hpp"

#include <algorithm>
#include <vector>

namespace mnc::meter {

class MeterAttributeSet {
public:
	MeterAttributeSet() = default;
	explicit MeterAttributeSet(std::vector<MeterAttributeKey> attributes);

	void add(MeterAttributeKey attribute);
	void add(MeterAttributeGroup group);

	[[nodiscard]] bool empty() const noexcept { return attributes_.empty(); }
	[[nodiscard]] bool contains(MeterAttributeKey attribute) const;
	[[nodiscard]] const std::vector<MeterAttributeKey> &values() const noexcept
	{
		return attributes_;
	}

private:
	std::vector<MeterAttributeKey> attributes_;
};

[[nodiscard]] std::vector<MeterAttributeKey>
attributes_in(MeterAttributeGroup group);

} // namespace mnc::meter
