#include "msap1/acquisition/meter_record_source.hpp"
#include "msap1/acquisition/rpu_control.hpp"
#include "msap1/acquisition_ipc.hpp"

#include <chrono>
#include <stdexcept>
#include <string_view>

namespace {

void require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

class FakeMeterSource final : public msap1::acquisition::MeterRecordSource {
public:
	void start() override { started = true; }
	void stop() noexcept override { started = false; }
	int native_handle() const noexcept override { return 17; }
	std::string_view name() const noexcept override { return "fake-meter"; }
	msap1::acquisition::MeterRecordBatch read_available() override
	{
		msap1::acquisition::MeterRecordBatch batch;
		batch.count = 1;
		batch.bytes = sizeof(msap1::MeterRecord);
		return batch;
	}
	bool started = false;
};

class FakeRpuControl final : public msap1::acquisition::RpuControl {
public:
	msap1::Message transact(
		std::uint8_t type, const void *, std::size_t,
		std::chrono::milliseconds) override
	{
		msap1::Message result;
		result.header.type = type;
		return result;
	}

	msap1_adc_health_payload query_health() override
	{
		msap1_adc_health_payload health{};
		health.sample_rate_hz = 32000;
		return health;
	}
};

void device_interfaces_are_substitutable()
{
	FakeMeterSource meter;
	msap1::acquisition::MeterRecordSource &source = meter;
	source.start();
	require(meter.started, "meter source did not start");
	require(source.native_handle() == 17, "wrong fake descriptor");
	require(source.read_available().count == 1, "wrong fake batch");
	source.stop();
	require(!meter.started, "meter source did not stop");

	FakeRpuControl fake;
	msap1::acquisition::RpuControl &rpu = fake;
	require(rpu.transact(9).header.type == 9, "wrong fake RPU response");
	require(rpu.query_health().sample_rate_hz == 32000,
		"wrong fake RPU health");
}

void product_codec_preserves_the_v14_contract()
{
	msap1::AcquisitionRequest request;
	request.command = msap1::AcquisitionCommand::meter_stream_read;
	request.sequence = 0x1122334455667788ull;
	request.meter_period = msap1::UpdatePeriod::s10;
	request.meter_cursor = 91;
	request.meter_limit = 7;
	request.meter_consumer = "historian";

	auto frame = msap1::acquisition::ProtocolCodec::encode_request(request);
	auto decoded = msap1::acquisition::ProtocolCodec::decode_request(frame);
	require(decoded.version == msap1::acquisition_ipc_version,
		"wrong IPC version");
	require(decoded.command == request.command, "wrong command");
	require(decoded.sequence == request.sequence, "wrong sequence");
	require(decoded.meter_period == request.meter_period, "wrong period");
	require(decoded.meter_cursor == request.meter_cursor, "wrong cursor");
	require(decoded.meter_limit == request.meter_limit, "wrong limit");
	require(decoded.meter_consumer == request.meter_consumer,
		"wrong consumer");

	msap1::AcquisitionResponse response;
	response.sequence = request.sequence;
	response.status = msap1::AcquisitionStatus::ok;
	response.sample_rate_hz = 64000;
	frame = msap1::acquisition::ProtocolCodec::encode_response(
		response, static_cast<std::uint32_t>(request.command));
	auto decoded_response =
		msap1::acquisition::ProtocolCodec::decode_response(frame);
	require(decoded_response.sequence == request.sequence,
		"wrong response sequence");
	require(decoded_response.sample_rate_hz == 64000,
		"wrong response sample rate");
}

} // namespace

int main()
{
	device_interfaces_are_substitutable();
	product_codec_preserves_the_v14_contract();
	return 0;
}
