#pragma once

#include "mnc/datalogger/types.hpp"

namespace mnc::datalogger {

class Clock {
public:
	virtual ~Clock() = default;
	[[nodiscard]] virtual UtcNanoseconds now() const noexcept = 0;
};

} // namespace mnc::datalogger
