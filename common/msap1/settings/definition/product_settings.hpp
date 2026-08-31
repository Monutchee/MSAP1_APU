#pragma once

#include "msap1/settings/definition/adc_settings.hpp"
#include "msap1/settings/definition/database_settings.hpp"
#include "msap1/settings/definition/data_logging_settings.hpp"
#include "msap1/settings/definition/metering_settings.hpp"
#include "msap1/settings/definition/modbus_settings.hpp"
#include "msap1/settings/definition/mqtt_settings.hpp"
#include "msap1/settings/definition/waveform_settings.hpp"

#include <cstdint>

namespace msap1::settings {

struct ProductSettings {
	//Constraints declaration
	static constexpr std::uint32_t supported_schema_version = 5;

	//Setting Payload
	std::uint32_t schema_version = supported_schema_version;
	MeteringSettings metering;
	AdcSettings adc;
	WaveformSettings waveform;
	DatabaseSettings database;
	ModbusSettings modbus;
	MqttSettings mqtt;
	DataLoggingSettings data_logging;

	/** Per-domain constraints plus the cross-domain meter configuration
	 *  check; the complete rule set for a persistable document. */
	void validate() const;
};

[[nodiscard]] MeterConversionFile
to_meter_configuration(const ProductSettings &settings);
[[nodiscard]] msap1_m18_config_payload
to_m18_configuration(const ProductSettings &settings,
	std::uint32_t configuration_generation);

} // namespace msap1::settings
