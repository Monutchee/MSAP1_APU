#include "msap1/acquisition/dma/meter_record_source.hpp"
#include "msap1/acquisition/rpu/rpu_control.hpp"
#include "msap1/acquisition/ipc/acquisition_ipc.hpp"

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

void typed_commands_round_trip_through_the_registry()
{
	msap1::AcquisitionCommandRegistry registry;
	registry.on<msap1::SampleRateSetRequest>(
		msap1::AcquisitionStatus::configuration_error,
		[](const msap1::SampleRateSetRequest &request) {
			require(request.sample_rate_hz == 64000,
				"wrong decoded sample rate");
			msap1::InfoResponse response{};
			response.running = true;
			response.sample_rate_hz = request.sample_rate_hz;
			msap1_adc_health_payload health{};
			health.drdy_frequency_hz = 63999;
			response.rpu_health = health;
			return response;
		});

	msap1::SampleRateSetRequest request;
	request.sample_rate_hz = 64000;
	auto frame = msap1::encode_acquisition_request(request);
	frame.correlation_id = 0x1122334455667788ull;
	require(frame.message_type ==
			msap1::acquisition_command_id<msap1::SampleRateSetRequest>,
		"wrong request message type");

	const auto reply = registry.dispatch(frame);
	require(reply.kind == mnc::ipc::FrameKind::response,
		"wrong reply frame kind");
	require(reply.message_type == frame.message_type,
		"reply must echo the command identity");
	require(reply.correlation_id == frame.correlation_id,
		"reply must echo the correlation ID");
	const auto response =
		msap1::decode_acquisition_payload<msap1::InfoResponse>(reply);
	require(response.status == msap1::AcquisitionStatus::ok, "wrong status");
	require(response.running, "wrong running flag");
	require(response.sample_rate_hz == 64000, "wrong response sample rate");
	require(response.rpu_health.value().drdy_frequency_hz == 63999,
		"wrong hardware-mirror payload round trip");
}

void malformed_and_unknown_requests_are_rejected()
{
	msap1::AcquisitionCommandRegistry registry;
	registry.on<msap1::InfoRequest>(msap1::AcquisitionStatus::dma_error,
		[](const msap1::InfoRequest &) {
			return msap1::InfoResponse{};
		});

	auto malformed = msap1::encode_acquisition_request(msap1::InfoRequest{});
	malformed.payload.resize(1);
	const auto rejected = registry.dispatch(malformed);
	require(rejected.kind == mnc::ipc::FrameKind::error,
		"malformed request must produce an error frame");
	const auto rejection =
		msap1::decode_acquisition_payload<msap1::InfoResponse>(rejected);
	require(rejection.status == msap1::AcquisitionStatus::bad_request,
		"malformed request must report bad_request");

	auto unknown = msap1::encode_acquisition_request(msap1::InfoRequest{});
	unknown.message_type = 0xdeadbeefu;
	const auto unknown_reply = registry.dispatch(unknown);
	require(unknown_reply.kind == mnc::ipc::FrameKind::error,
		"unknown command must produce an error frame");
}

} // namespace

int main()
{
	device_interfaces_are_substitutable();
	typed_commands_round_trip_through_the_registry();
	malformed_and_unknown_requests_are_rejected();
	return 0;
}
