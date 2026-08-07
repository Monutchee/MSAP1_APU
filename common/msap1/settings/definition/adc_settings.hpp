#pragma once

#include "msap1/meter/meter_config.hpp"

#include <string>

namespace msap1::settings {

/** The source value is cross-checked against the assembled meter
 *  configuration in ProductSettings::validate(). */
struct AdcSettings {
	std::string source;
	SimulatorConfig simulator;
};

} // namespace msap1::settings
