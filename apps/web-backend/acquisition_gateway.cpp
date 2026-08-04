#include "acquisition_gateway.hpp"

namespace msap1::web {

AcquisitionResponse AcquisitionGateway::information(int timeout_ms)
{
	return client_.request(AcquisitionCommand::info, timeout_ms);
}

AcquisitionResponse AcquisitionGateway::waveform_status(int timeout_ms)
{
	return client_.request(AcquisitionCommand::waveform_status, timeout_ms);
}

AcquisitionResponse AcquisitionGateway::trigger_waveform(
	std::uint32_t pretrigger_ms, std::uint32_t posttrigger_ms,
	WaveformTriggerSource source, int timeout_ms)
{
	return client_.request(AcquisitionCommand::waveform_trigger, timeout_ms,
		nullptr, 0, 0, pretrigger_ms, posttrigger_ms, source);
}

AcquisitionResponse AcquisitionGateway::delete_waveform(
	std::uint64_t session_id, int timeout_ms)
{
	return client_.request(AcquisitionCommand::waveform_delete, timeout_ms,
		nullptr, 0, 0, 0, 0, WaveformTriggerSource::manual_web,
		session_id);
}

AcquisitionResponse AcquisitionGateway::frequency_configuration(int timeout_ms)
{
	return client_.request(
		AcquisitionCommand::frequency_configuration_get, timeout_ms);
}

AcquisitionResponse AcquisitionGateway::set_frequency_configuration(
	const FrequencyIpcConfiguration &configuration, int timeout_ms)
{
	return client_.request(
		AcquisitionCommand::frequency_configuration_set, timeout_ms,
		&configuration);
}

AcquisitionResponse AcquisitionGateway::adc_source(int timeout_ms)
{
	return client_.request(AcquisitionCommand::adc_source_get, timeout_ms);
}

AcquisitionResponse AcquisitionGateway::set_adc_source(
	std::uint32_t source, int timeout_ms)
{
	return client_.request(AcquisitionCommand::adc_source_set, timeout_ms,
		nullptr, 0, 0, 10000, 10000,
		WaveformTriggerSource::manual_cli, 0, source);
}

AcquisitionResponse AcquisitionGateway::simulator_configuration(int timeout_ms)
{
	return client_.request(
		AcquisitionCommand::adc_simulator_get, timeout_ms);
}

AcquisitionResponse AcquisitionGateway::set_simulator_configuration(
	const SimulatorIpcConfiguration &configuration, int timeout_ms)
{
	return client_.request(AcquisitionCommand::adc_simulator_set, timeout_ms,
		nullptr, 0, 0, 10000, 10000,
		WaveformTriggerSource::manual_cli, 0,
		MSAP1_ADC_SOURCE_PHYSICAL, &configuration);
}

AcquisitionResponse AcquisitionGateway::set_capture(bool enabled, int timeout_ms)
{
	return client_.request(
		enabled ? AcquisitionCommand::start : AcquisitionCommand::stop,
		timeout_ms);
}

} // namespace msap1::web
