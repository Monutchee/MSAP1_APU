#include "mnc/mqtt/mqtt_client.hpp"
#include "msap1/mqtt/meter_publication_catalog.hpp"
#include "msap1/mqtt/meter_publication_scheduler.hpp"
#include "msap1/mqtt/meter_snapshot_payload_encoder.hpp"
#include "msap1/settings/definition/mqtt_settings.hpp"

#include <boost/asio/io_context.hpp>

#include <array>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;

void require(bool condition)
{
	if (!condition)
		throw std::runtime_error("MQTT test assertion failed");
}

class FakeSnapshotProvider final : public mnc::meter::MeterSnapshotProvider {
public:
	std::vector<mnc::meter::MeterCapabilities> capabilities() const override
	{
		return {{mnc::meter::MeasurementPeriod::Basic,
			{{mnc::meter::MeterAttributeId::Frequency, std::nullopt},
			 {mnc::meter::MeterAttributeId::VanRms, std::nullopt}}}};
	}

	std::optional<mnc::meter::MeterSnapshot> latest(
		const mnc::meter::MeterSnapshotRequest &request) const override
	{
		mnc::meter::MeterSnapshot snapshot;
		snapshot.period = request.period;
		snapshot.sequence = ++sequence_;
		snapshot.configuration_generation = 0x12345678;
		mnc::meter::MeterSnapshotTiming timing;
		timing.quality = mnc::meter::TimeQuality::Synchronized;
		timing.utc_start_nanoseconds =
			1'786'646'400'000'000'000LL;
		timing.sample_count = 6'400;
		snapshot.timing = timing;
		for (const auto attribute : request.attributes) {
			const auto descriptor = mnc::meter::describe(attribute);
			snapshot.values.push_back({
				.attribute = attribute,
				.unit = descriptor.unit,
				.quality = attribute.id ==
					mnc::meter::MeterAttributeId::Frequency
					? mnc::meter::ReadingQuality::Valid
					: mnc::meter::ReadingQuality::Unavailable,
				.value = 60'001,
				.source_sequence = snapshot.sequence,
				.calculation_window_nanoseconds = 200'000'000});
		}
		return snapshot;
	}

	mnc::meter::LatestSubscription subscribe_latest(
		const mnc::meter::MeterSnapshotRequest &, Callback) override
	{
		return {};
	}

private:
	mutable std::uint64_t sequence_ = 40;
};

class FakeMqttClient final : public mnc::mqtt::MqttClient {
public:
	void configure(const mnc::mqtt::ConnectionOptions &) override {}
	void connect() override { connected = true; }
	void disconnect() noexcept override { connected = false; }
	void publish(const mnc::mqtt::PublishRequest &request) override
	{
		if (!connected)
			throw std::runtime_error("offline");
		published.push_back(request);
	}
	mnc::mqtt::ConnectionStatus status() const override
	{
		mnc::mqtt::ConnectionStatus result;
		result.state = connected ? mnc::mqtt::ConnectionState::connected
			: mnc::mqtt::ConnectionState::disconnected;
		return result;
	}

	bool connected = false;
	std::vector<mnc::mqtt::PublishRequest> published;
};

void test_server_uris()
{
	mnc::mqtt::ConnectionOptions options;
	options.host = "broker.local";
	options.port = 1883;
	require(mnc::mqtt::server_uri(options) == "mqtt://broker.local:1883");
	options.transport = mnc::mqtt::Transport::mqtts;
	options.port = 8883;
	require(mnc::mqtt::server_uri(options) == "mqtts://broker.local:8883");
	options.transport = mnc::mqtt::Transport::ws;
	options.port = 80;
	options.websocket_path = "/mqtt";
	require(mnc::mqtt::server_uri(options) == "ws://broker.local:80/mqtt");
	options.transport = mnc::mqtt::Transport::wss;
	options.port = 443;
	options.host = "2001:db8::1";
	require(mnc::mqtt::server_uri(options) == "wss://[2001:db8::1]:443/mqtt");
}

void test_payload_quality_and_units()
{
	FakeSnapshotProvider provider;
	const std::vector<mnc::meter::MeterAttributeKey> attributes{
		{mnc::meter::MeterAttributeId::Frequency, std::nullopt},
		{mnc::meter::MeterAttributeId::VanRms, std::nullopt}};
	const auto snapshot = provider.latest(
		{mnc::meter::MeasurementPeriod::Basic, attributes});
	require(snapshot.has_value());
	const auto json = msap1::mqtt::MeterSnapshotPayloadEncoder{}.encode(
		*snapshot, "fundamental", attributes);
	require(json.find("\"schema\":\"mnc.meter.snapshot.v1\"") !=
		std::string::npos);
	require(json.find("\"frequency\":{\"value\":60.001") !=
		std::string::npos);
	require(json.find("\"unit\":\"Hz\"") != std::string::npos);
	require(json.find("\"voltage.ln.a.rms\":{\"value\":null") !=
		std::string::npos);
	require(json.find("\"quality\":\"unavailable\"") !=
		std::string::npos);
}

void test_exact_energy_payload_and_metadata()
{
	mnc::meter::MeterSnapshot snapshot;
	snapshot.period = mnc::meter::MeasurementPeriod::Basic;
	snapshot.energy = mnc::meter::EnergySnapshotMetadata{
		.session_id = 0xfedcba9876543210ULL,
		.reset_epoch = 9,
		.last_sample_index = 100,
		.accepted_samples = 90,
		.skipped_samples = 10,
		.accepted_blocks = 8,
		.skipped_blocks = 1,
		.incomplete_input = true,
		.discontinuity = true,
	};
	const mnc::meter::MeterAttributeKey attribute{
		mnc::meter::MeterAttributeId::ReactiveEnergyQuadrantIVTotal,
		std::nullopt};
	snapshot.values.push_back({
		.attribute = attribute,
		.unit = mnc::meter::MeterUnit::MicroVarHours,
		.quality = mnc::meter::ReadingQuality::Valid,
		.value = 9007199254740993LL,
		.source_sequence = 7,
	});
	const std::array selected{attribute};
	const auto json = msap1::mqtt::MeterSnapshotPayloadEncoder{}.encode(
		snapshot, "energy", selected);
	require(json.find("\"value\":\"9007199254740993\"") !=
		std::string::npos);
	require(json.find("\"session_id\":\"18364758544493064720\"") !=
		std::string::npos);
	require(json.find("\"reset_epoch\":\"9\"") != std::string::npos);
	require(json.find("\"discontinuity\":true") != std::string::npos);
	require(json.find("energy.reactive.quadrant_iv.total") !=
		std::string::npos);
}

void test_catalog_and_newest_pending_payload()
{
	boost::asio::io_context context;
	FakeSnapshotProvider provider;
	FakeMqttClient client;
	const auto catalog = msap1::mqtt::MeterPublicationCatalog::capabilities(
		provider);
	require(catalog.size() == 1);
	require(catalog.front().attributes.size() == 2);

	msap1::mqtt::MeterPublicationScheduler scheduler{
		context.get_executor(), provider, client};
	msap1::settings::MqttPublicationSettings publication{
		.id = "frequency", .enabled = true, .topic = "msap1/frequency",
		.period = "basic", .interval_ms = 100, .qos = 1,
		.retain = false, .attributes = {"frequency"}};
	scheduler.configure({publication});
	scheduler.start();
	context.run_for(240ms);
	require(client.published.empty());
	const auto before = scheduler.statistics().at("frequency");
	require(before.attempts >= 2);
	require(before.failures >= 2);

	client.connect();
	scheduler.flush_pending();
	require(client.published.size() == 1);
	require(client.published.front().qos == 1);
	require(!client.published.front().retain);
	/* Only the newest offline snapshot is replayed, not every missed tick. */
	require(client.published.front().payload.find(
		"\"sequence\":" + std::to_string(before.last_source_sequence)) !=
		std::string::npos);
	scheduler.stop();
}

void test_settings_validation()
{
	msap1::settings::MqttSettings settings;
	settings.enabled = true;
	settings.connection.broker_host = "broker.local";
	settings.connection.client_id = "meter-1";
	settings.publications = {{
		.id = "voltage", .enabled = true, .topic = "msap1/voltage",
		.period = "basic", .interval_ms = 100, .qos = 2,
		.retain = true, .attributes = {"voltage.ln.a.rms"}}};
	settings.validate();

	auto invalid = settings;
	invalid.publications.front().topic = "msap1/+";
	bool rejected = false;
	try {
		invalid.validate();
	} catch (const std::runtime_error &) {
		rejected = true;
	}
	require(rejected);

	invalid = settings;
	invalid.connection.transport = msap1::settings::MqttTransport::wss;
	invalid.connection.websocket_path = "mqtt";
	rejected = false;
	try {
		invalid.validate();
	} catch (const std::runtime_error &) {
		rejected = true;
	}
	require(rejected);

	invalid = settings;
	invalid.connection.transport = msap1::settings::MqttTransport::mqtts;
	invalid.tls.verify_peer = false;
	invalid.tls.verify_hostname = true;
	rejected = false;
	try {
		invalid.validate();
	} catch (const std::runtime_error &) {
		rejected = true;
	}
	require(rejected);

	/* TLS preferences are deliberately retained while plain MQTT is active. */
	settings.tls.use_client_certificate = true;
	settings.validate();
}

} // namespace

int main()
{
	test_server_uris();
	test_payload_quality_and_units();
	test_exact_energy_payload_and_metadata();
	test_catalog_and_newest_pending_payload();
	test_settings_validation();
}
