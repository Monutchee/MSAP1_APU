#include "gateway/acquisition_gateway.hpp"

#include <utility>

namespace msap1::web {

InfoResponse AcquisitionGateway::information(int timeout_ms)
{
	return client_.request(InfoRequest{}, timeout_ms);
}

MeterSnapshotResponse AcquisitionGateway::meter_snapshot(
	mnc::meter::MeterSnapshotRequest selection, int timeout_ms)
{
	MeterSnapshotRequest request;
	request.selection = std::move(selection);
	return client_.request(request, timeout_ms);
}

WaveformResponse AcquisitionGateway::waveform_status(int timeout_ms)
{
	return client_.request(WaveformStatusRequest{}, timeout_ms);
}

WaveformResponse AcquisitionGateway::waveform_list(
	const WaveformListRequest &request, int timeout_ms)
{
	return client_.request(request, timeout_ms);
}

WaveformLookupResponse AcquisitionGateway::waveform_lookup(
	const WaveformLookupRequest &request, int timeout_ms)
{
	return client_.request(request, timeout_ms);
}

WaveformResponse AcquisitionGateway::trigger_waveform(
	std::uint32_t pretrigger_ms, std::uint32_t posttrigger_ms,
	std::uint32_t decimation, WaveformTriggerSource source, int timeout_ms)
{
	WaveformTriggerRequest request;
	request.pretrigger_ms = pretrigger_ms;
	request.posttrigger_ms = posttrigger_ms;
	request.decimation = decimation;
	request.source = source;
	return client_.request(request, timeout_ms);
}

WaveformResponse AcquisitionGateway::delete_waveform(std::uint64_t session_id,
						     int timeout_ms)
{
	WaveformDeleteRequest request;
	request.session_id = session_id;
	return client_.request(request, timeout_ms);
}

FrequencyResponse AcquisitionGateway::frequency_configuration(int timeout_ms)
{
	return client_.request(FrequencyGetRequest{}, timeout_ms);
}

AdcSourceResponse AcquisitionGateway::adc_source(int timeout_ms)
{
	return client_.request(AdcSourceGetRequest{}, timeout_ms);
}

SimulatorResponse AcquisitionGateway::simulator_configuration(int timeout_ms)
{
	return client_.request(SimulatorGetRequest{}, timeout_ms);
}

SingleCycleResponse AcquisitionGateway::single_cycle(int timeout_ms)
{
	return client_.request(SingleCycleRequest{}, timeout_ms);
}

PowerQualityResponse AcquisitionGateway::power_quality(int timeout_ms)
{
	return client_.request(PowerQualityRequest{}, timeout_ms);
}

FlickerResponse AcquisitionGateway::flicker(int timeout_ms)
{
	return client_.request(FlickerRequest{}, timeout_ms);
}

MainsSignalResponse AcquisitionGateway::mains_signalling(int timeout_ms)
{
	return client_.request(MainsSignalRequest{}, timeout_ms);
}

HarmonicResponse AcquisitionGateway::harmonics(
	mnc::meter::MeasurementPeriod period, int timeout_ms)
{
	HarmonicRequest request{};
	request.period = period;
	return client_.request(request, timeout_ms);
}

SimulatorEventResponse AcquisitionGateway::simulator_event(
	const SimulatorEventRequest &request, int timeout_ms)
{
	return client_.request(request, timeout_ms);
}

CaptureResponse AcquisitionGateway::set_capture(bool enabled, int timeout_ms)
{
	if (enabled)
		return client_.request(StartRequest{}, timeout_ms);
	return client_.request(StopRequest{}, timeout_ms);
}

} // namespace msap1::web
