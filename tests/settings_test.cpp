#include "msap1/settings/settings.hpp"
#include "msap1/settings/settings_ipc.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

namespace {

using msap1::settings::ProductSettings;
using msap1::settings::SettingsHandler;

void require(bool condition, std::string_view message)
{
	if (!condition)
		throw std::runtime_error(std::string(message));
}

struct TestTree {
	std::filesystem::path root;
	std::filesystem::path data;
	std::filesystem::path factory;

	explicit TestTree(std::string_view name)
	{
		root = std::filesystem::temp_directory_path() /
			("msap1-settings-" + std::string(name) + "-" +
			 std::to_string(::getpid()));
		data = root / "data";
		factory = root / "factory.json";
		std::filesystem::create_directories(root);
		std::filesystem::copy_file(
			std::filesystem::path(PROJECT_SOURCE_DIR) /
				"config/settings/factory-defaults.json",
			factory, std::filesystem::copy_options::overwrite_existing);
	}

	~TestTree()
	{
		std::error_code ignored;
		std::filesystem::remove_all(root, ignored);
	}
};

void test_first_boot_and_direct_save()
{
	TestTree tree("first-boot");
	{
		SettingsHandler handler(tree.data, tree.factory);
		handler.initialize();
		const auto initial = handler.active();
		assert(initial.settings.schema_version == 5u);
		assert(initial.settings.metering.events.voltage_sag.enabled);
		assert(initial.settings.metering.events.voltage_sag.waveform.decimation == 8u);
		assert(initial.settings.metering.events.voltage_swell.waveform.decimation == 8u);
		assert(initial.settings.metering.events.voltage_interruption.waveform.decimation == 8u);
		assert(initial.settings.metering.events.rapid_voltage_change.waveform.decimation == 8u);
		assert(!initial.settings.metering.events.current_sag.enabled);
		assert(initial.settings.metering.events.voltage_unbalance.waveform.decimation == 8u);
		assert(initial.settings.metering.events.current_sag.waveform.decimation == 8u);
		assert(initial.settings.metering.events.current_swell.waveform.decimation == 8u);
		assert(initial.settings.metering.events.current_unbalance.waveform.decimation == 8u);
		assert(!initial.settings.metering.events.transient_voltage.enabled);
		assert(initial.settings.metering.events.transient_voltage.waveform.decimation == 8u);
		assert(initial.settings.metering.flicker.pst_interval_seconds == 600u);
		assert(!initial.settings.metering.mains_signalling.enabled);
		assert(initial.settings.metering.sample_rate_hz == 128000u);
		assert(initial.settings.waveform.default_decimation == 1u);
		assert(initial.settings.metering.measurement_topology == "wye");
		assert(initial.settings.metering.system_nominal_voltage_v == 120.0);
		assert(initial.settings.metering.demand.method == "sliding");
		assert(initial.settings.metering.demand.window_seconds == 60u);
		assert(initial.settings.database.demand.backend == "persistent");
		assert(!initial.content_hash.empty());
		assert(std::filesystem::exists(tree.data / "active.json"));

		auto settings = initial.settings;
		settings.metering.frequency.maximum_hz = 80.0;
		const auto saved = handler.save(settings);
		assert(saved.settings.metering.frequency.maximum_hz == 80.0);
		assert(saved.content_hash != initial.content_hash);
		assert(!std::filesystem::exists(tree.data / "draft.json"));
		assert(!std::filesystem::exists(tree.data / "pending.json"));
		assert(!std::filesystem::exists(tree.data / "revisions"));
	}

	SettingsHandler reloaded(tree.data, tree.factory);
	reloaded.initialize();
	assert(reloaded.active().settings.metering.frequency.maximum_hz == 80.0);
}

void test_demand_profile_validation()
{
	TestTree tree("demand-profile");
	SettingsHandler handler(tree.data, tree.factory);
	handler.initialize();
	auto settings = handler.active().settings;
	settings.metering.demand = {"sliding", 300u};
	auto saved = handler.save(settings);
	assert(saved.settings.metering.demand.method == "sliding");
	assert(saved.settings.metering.demand.window_seconds == 300u);
	settings = saved.settings;
	settings.metering.demand = {"fixed_block", 600u};
	saved = handler.save(settings);
	assert(saved.settings.metering.demand.method == "fixed_block");

	for (const auto &invalid : {
		msap1::settings::DemandSettings{"sliding", 61u},
		msap1::settings::DemandSettings{"fixed_block", 300u},
		msap1::settings::DemandSettings{"unknown", 60u}}) {
		settings = saved.settings;
		settings.metering.demand = invalid;
		bool rejected = false;
		try {
			(void)handler.save(settings);
		} catch (const std::runtime_error &) {
			rejected = true;
		}
		assert(rejected);
	}
}

void test_power_quality_settings_and_wire_snapshot()
{
	TestTree tree("power-quality-contract");
	SettingsHandler handler(tree.data, tree.factory);
	handler.initialize();
	auto settings = handler.active().settings;
	settings.metering.power_quality.reference_volts = 120.0;
	settings.metering.events.voltage_sag.threshold_percent = 88.5;
	settings.metering.events.current_sag.enabled = true;
	settings.metering.events.reference_current_amperes = 5.0;
	settings.metering.events.current_sag.waveform.enabled = true;
	settings.metering.flicker.lamp_voltage = 230u;
	settings.metering.mains_signalling.enabled = true;
	settings.metering.mains_signalling.carrier_frequency_hz = 1000.0;
	settings.waveform.station_id = "station-a";
	settings.waveform.site_id = "site-a";
	settings.waveform.circuit_id = "circuit-a";
	const auto saved = handler.save(settings);
	const auto prepared = msap1::prepare_meter_configuration(
		msap1::settings::to_meter_configuration(saved.settings),
		saved.settings.metering.sample_rate_hz);
	const auto wire = msap1::settings::to_m18_configuration(
		saved.settings, prepared.wire.generation);
	assert(wire.generation == prepared.wire.generation);
	assert(wire.event_profile_count == MSAP1_M18_EVENT_TYPE_COUNT);
	assert(wire.event[MSAP1_M18_EVENT_VOLTAGE_SAG].threshold_e4 == 8850u);
	assert((wire.event[MSAP1_M18_EVENT_CURRENT_SAG].flags &
		MSAP1_M18_EVENT_ENABLED) != 0u);
	assert(wire.reference_current_microamperes == 5000000u);
	assert(wire.reference_voltage_microvolts == 120000000u);
	assert(wire.flicker_lamp_voltage == 230u);
	assert(wire.mains_carrier_millihz == 1000000u);

	settings = saved.settings;
	settings.metering.events.transient_voltage.enabled = true;
	bool transient_rejected = false;
	try {
		(void)handler.save(settings);
	} catch (const std::runtime_error &) {
		transient_rejected = true;
	}
	assert(transient_rejected);

	settings = saved.settings;
	settings.metering.mains_signalling.carrier_frequency_hz = 12490.0;
	settings.metering.mains_signalling.bandwidth_hz = 20.0;
	bool frontend_band_rejected = false;
	try {
		(void)handler.save(settings);
	} catch (const std::runtime_error &) {
		frontend_band_rejected = true;
	}
	assert(frontend_band_rejected);

	settings = saved.settings;
	settings.metering.sample_rate_hz = 2000u;
	settings.metering.mains_signalling.carrier_frequency_hz = 990.0;
	settings.metering.mains_signalling.bandwidth_hz = 20.0;
	bool nyquist_band_rejected = false;
	try {
		(void)handler.save(settings);
	} catch (const std::runtime_error &) {
		nyquist_band_rejected = true;
	}
	assert(nyquist_band_rejected);
}

void test_nominal_frequency_validation()
{
	TestTree tree("nominal-frequency");
	SettingsHandler handler(tree.data, tree.factory);
	handler.initialize();
	/* The factory document must carry the explicit default. */
	assert(handler.active().settings.metering.nominal_frequency_hz == 60u);

	/* Both supported grids persist; anything else is rejected. */
	auto settings = handler.active().settings;
	settings.metering.nominal_frequency_hz = 50u;
	assert(handler.save(settings).settings.metering.nominal_frequency_hz ==
	       50u);
	settings.metering.nominal_frequency_hz = 60u;
	assert(handler.save(settings).settings.metering.nominal_frequency_hz ==
	       60u);
	settings.metering.nominal_frequency_hz = 55u;
	[[maybe_unused]] bool rejected = false;
	try {
		(void)handler.save(settings);
	} catch (const std::runtime_error &) {
		rejected = true;
	}
	assert(rejected);
	assert(handler.active().settings.metering.nominal_frequency_hz == 60u);
}

void test_system_nominal_voltage_validation()
{
	TestTree tree("system-nominal-voltage");
	SettingsHandler handler(tree.data, tree.factory);
	handler.initialize();
	assert(handler.active().settings.metering.system_nominal_voltage_v ==
	       120.0);

	auto settings = handler.active().settings;
	settings.metering.system_nominal_voltage_v = 230.0;
	assert(handler.save(settings)
		       .settings.metering.system_nominal_voltage_v == 230.0);
	settings.metering.system_nominal_voltage_v = 0.0;
	[[maybe_unused]] bool rejected = false;
	try {
		(void)handler.save(settings);
	} catch (const std::runtime_error &) {
		rejected = true;
	}
	assert(rejected);
	assert(handler.active().settings.metering.system_nominal_voltage_v ==
	       230.0);
}

void test_measurement_topology_validation()
{
	TestTree tree("measurement-topology");
	SettingsHandler handler(tree.data, tree.factory);
	handler.initialize();
	assert(handler.active().settings.metering.measurement_topology == "wye");

	auto settings = handler.active().settings;
	settings.metering.measurement_topology = "delta";
	const auto saved = handler.save(settings);
	assert(saved.settings.metering.measurement_topology == "delta");
	assert(msap1::settings::SettingsCodec::encode(saved.settings, false)
		       .find("\"measurement_topology\":\"delta\"") !=
	       std::string::npos);

	settings.metering.measurement_topology = "open-delta";
	[[maybe_unused]] bool rejected = false;
	try {
		(void)handler.save(settings);
	} catch (const std::runtime_error &) {
		rejected = true;
	}
	assert(rejected);
	assert(handler.active().settings.metering.measurement_topology == "delta");
}

void test_existing_settings_default_system_nominal_voltage()
{
	TestTree tree("existing-system-nominal-voltage");
	std::ifstream input(tree.factory);
	std::string json((std::istreambuf_iterator<char>(input)),
			 std::istreambuf_iterator<char>());
	const std::string schema = "\"schema_version\": 5";
	const auto schema_position = json.find(schema);
	assert(schema_position != std::string::npos);
	json.replace(schema_position, schema.size(), "\"schema_version\": 2");
	const std::string topology =
		"    \"measurement_topology\": \"wye\",\n";
	const auto topology_position = json.find(topology);
	assert(topology_position != std::string::npos);
	json.erase(topology_position, topology.size());
	const std::string member =
		"    \"system_nominal_voltage_v\": 120.0,\n";
	const auto member_position = json.find(member);
	assert(member_position != std::string::npos);
	json.erase(member_position, member.size());

	const std::string original_pretrigger =
		"\"default_pretrigger_ms\": 3000";
	const auto pretrigger_position = json.find(original_pretrigger);
	assert(pretrigger_position != std::string::npos);
	json.replace(pretrigger_position, original_pretrigger.size(),
		     "\"default_pretrigger_ms\": 4321");
	const std::string original_sag = "\"sag_percent\": 90.0";
	const auto sag_position = json.find(original_sag);
	assert(sag_position != std::string::npos);
	json.replace(sag_position, original_sag.size(), "\"sag_percent\": 87.5");
	const std::string original_swell = "\"swell_percent\": 110.0";
	const auto swell_position = json.find(original_swell);
	assert(swell_position != std::string::npos);
	json.replace(swell_position, original_swell.size(),
		     "\"swell_percent\": 112.5");
	const std::string original_interruption =
		"\"interruption_percent\": 10.0";
	const auto interruption_position = json.find(original_interruption);
	assert(interruption_position != std::string::npos);
	json.replace(interruption_position, original_interruption.size(),
		     "\"interruption_percent\": 7.5");
	const std::string original_hysteresis =
		"\"hysteresis_percent\": 2.0";
	const auto hysteresis_position = json.find(original_hysteresis);
	assert(hysteresis_position != std::string::npos);
	json.replace(hysteresis_position, original_hysteresis.size(),
		     "\"hysteresis_percent\": 1.5");
	std::filesystem::create_directories(tree.data);
	std::ofstream(tree.data / "active.json") << json;

	SettingsHandler handler(tree.data, tree.factory);
	handler.initialize();
	assert(handler.active().settings.schema_version == 5u);
	assert(handler.active().settings.waveform.default_pretrigger_ms == 4321u);
	assert(handler.active().settings.metering.measurement_topology == "wye");
	assert(handler.active().settings.metering.system_nominal_voltage_v ==
	       120.0);
	const auto &events = handler.active().settings.metering.events;
	assert(events.voltage_sag.threshold_percent == 87.5);
	assert(events.voltage_swell.threshold_percent == 112.5);
	assert(events.voltage_interruption.threshold_percent == 7.5);
	assert(events.voltage_sag.hysteresis_percent == 1.5);
	assert(events.voltage_swell.hysteresis_percent == 1.5);
	assert(events.voltage_interruption.hysteresis_percent == 1.5);
	assert(!events.current_sag.enabled);
	assert(!events.voltage_unbalance.enabled);
	assert(!events.transient_voltage.enabled);
}

void test_failed_apply_preserves_active()
{
	TestTree tree("rollback");
	std::uint32_t runtime_pretrigger = 3000u;
	msap1::settings::SettingsApplyCoordinator coordinator{
		[&](const ProductSettings &settings) {
			if (settings.waveform.default_pretrigger_ms == 7777u)
				throw std::runtime_error("injected apply failure");
			runtime_pretrigger = settings.waveform.default_pretrigger_ms;
		}};
	SettingsHandler handler(tree.data, tree.factory, std::move(coordinator));
	handler.initialize();
	auto settings = handler.active().settings;
	settings.waveform.default_pretrigger_ms = 7777u;
	[[maybe_unused]] bool failed = false;
	try {
		(void)handler.save(settings);
	} catch (const std::runtime_error &) {
		failed = true;
	}
	assert(failed);
	assert(handler.active().settings.waveform.default_pretrigger_ms == 3000u);
	assert(runtime_pretrigger == 3000u);
}

void test_empty_active_recovers_factory_defaults()
{
	TestTree tree("empty-active");
	{
		SettingsHandler handler(tree.data, tree.factory);
		handler.initialize();
		auto settings = handler.active().settings;
		settings.waveform.default_pretrigger_ms = 4321u;
		(void)handler.save(settings);
	}
	std::ofstream(tree.data / "active.json", std::ios::trunc);

	SettingsHandler recovered(tree.data, tree.factory);
	recovered.initialize();
	assert(!recovered.recovery_mode());
	assert(recovered.active().settings.waveform.default_pretrigger_ms == 3000u);
}

void test_invalid_active_recovers_factory_defaults()
{
	TestTree tree("recovery");
	{
		SettingsHandler handler(tree.data, tree.factory);
		handler.initialize();
		auto settings = handler.active().settings;
		settings.waveform.default_posttrigger_ms = 4321u;
		(void)handler.save(settings);
	}
	{
		std::ofstream output(tree.data / "active.json", std::ios::trunc);
		output << "{not-valid-json";
	}
	{
		SettingsHandler recovered(tree.data, tree.factory);
		recovered.initialize();
		assert(!recovered.recovery_mode());
		assert(recovered.active().settings.waveform.default_posttrigger_ms ==
			3000u);
	}

	SettingsHandler reloaded(tree.data, tree.factory);
	reloaded.initialize();
	assert(!reloaded.recovery_mode());
	assert(reloaded.active().settings.waveform.default_posttrigger_ms == 3000u);
}

void test_secrets_and_factory_reset()
{
	TestTree tree("factory-reset");
	SettingsHandler handler(tree.data, tree.factory);
	handler.initialize();
	handler.set_secret_document(R"({"mqtt_password":"not-public"})");
	assert(handler.has_secrets());
	[[maybe_unused]] struct stat status {};
	assert(::stat((tree.data / "secrets.json").c_str(), &status) == 0);
	assert((status.st_mode & 0777u) == 0600u);

	auto settings = handler.active().settings;
	settings.waveform.default_posttrigger_ms = 1234u;
	(void)handler.save(settings);
	(void)handler.factory_reset(true);
	assert(handler.active().settings.waveform.default_posttrigger_ms == 3000u);
	assert(!handler.has_secrets());
}

void test_schema_four_migrates_to_empty_data_logging()
{
	TestTree tree("schema-four-migration");
	std::ifstream input(tree.factory);
	std::string json((std::istreambuf_iterator<char>(input)),
		std::istreambuf_iterator<char>());
	const auto version = json.find("\"schema_version\": 5");
	require(version != std::string::npos, "factory schema marker is missing");
	json.replace(version, std::string_view{"\"schema_version\": 5"}.size(),
		"\"schema_version\": 4");
	const auto data_logging = json.find(",\n  \"data_logging\"");
	require(data_logging != std::string::npos,
		"factory data_logging section is missing");
	const auto root_close = json.rfind("\n}");
	require(root_close != std::string::npos && root_close > data_logging,
		"factory root closing brace is missing");
	json.erase(data_logging, root_close - data_logging);

	const auto migrated = msap1::settings::SettingsCodec::decode(json);
	require(migrated.schema_version == 5u,
		"schema-four document did not migrate to schema five");
	require(migrated.data_logging.channels.empty() &&
		migrated.data_logging.jobs.empty(),
		"schema migration enabled outbound traffic");
	require(migrated.metering.events.voltage_sag.enabled &&
		migrated.waveform.default_pretrigger_ms == 3000u,
		"schema migration changed an M18 setting");
}

void test_data_logging_configuration_validation()
{
	using namespace msap1::settings;
	TestTree tree("data-logging-validation");
	SettingsHandler handler(tree.data, tree.factory);
	handler.initialize();
	auto settings = handler.active().settings;
	DataChannelSettings channel;
	channel.id = "2ee37d86-4625-4f25-9d50-27cbd734d189";
	channel.name = "Operations HTTPS";
	channel.enabled = true;
	channel.protocol = DataChannelProtocol::https;
	channel.host = "collector.example.test";
	channel.port = 0;
	channel.http_path = "/meter-data";
	channel.authentication = DataChannelAuthentication::bearer;
	settings.data_logging.channels = {channel};
	settings.data_logging.jobs = {{
		.id = "10f9b506-2ff7-48ac-bc8a-7a12e359cd83",
		.name = "Five minute meter data",
		.enabled = true,
		.revision = 1,
		.source_period = "basic",
		.generation_interval_seconds = 300,
		.row_interval_seconds = 60,
		.selections = {{"voltage.ln.a.rms", "minimum"},
			{"voltage.ln.a.rms", "maximum"},
			{"voltage.ln.a.rms", "average"}},
		.format = "json",
		.destination = DataLoggingDestination::remote,
		.channel_ids = {channel.id},
	}};
	const auto saved = handler.save(settings);
	require(saved.settings.data_logging.jobs.size() == 1,
		"valid data logging job was not persisted");

	auto invalid = saved.settings;
	invalid.data_logging.channels.front().protocol = DataChannelProtocol::http;
	invalid.data_logging.channels.front().insecure_transport_acknowledged = false;
	bool rejected = false;
	try {
		(void)handler.save(invalid);
	} catch (const std::runtime_error &) {
		rejected = true;
	}
	require(rejected, "clear-text HTTP was accepted without acknowledgement");

	invalid = saved.settings;
	invalid.data_logging.jobs.front().destination =
		DataLoggingDestination::local_only;
	rejected = false;
	try {
		(void)handler.save(invalid);
	} catch (const std::runtime_error &) {
		rejected = true;
	}
	require(rejected, "Local-only job accepted a remote channel");

	invalid = saved.settings;
	invalid.data_logging.jobs.front().source_period = "minutes_10";
	invalid.data_logging.jobs.front().row_interval_seconds = 60;
	rejected = false;
	try {
		(void)handler.save(invalid);
	} catch (const std::runtime_error &) {
		rejected = true;
	}
	require(rejected, "10-minute source accepted a one-minute row");
}

void test_channel_scoped_credentials_and_assets()
{
	TestTree tree("channel-secrets");
	SettingsHandler handler(tree.data, tree.factory);
	handler.initialize();
	const std::string id = "2ee37d86-4625-4f25-9d50-27cbd734d189";
	const auto prefix = "data-channel." + id + ".";
	handler.set_secret(prefix + "bearer-token", "secret-token");
	require(handler.has_secret(prefix + "bearer-token"),
		"channel bearer token presence was lost");
	require(handler.runtime_secret(prefix + "bearer-token") == "secret-token",
		"runtime channel token resolution failed");
	handler.put_asset(prefix + "ca",
		"-----BEGIN CERTIFICATE-----\ntest\n-----END CERTIFICATE-----\n");
	handler.put_asset(prefix + "known-hosts",
		"example.test ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAITest\n");
	require(handler.has_asset(prefix + "ca") &&
		handler.has_asset(prefix + "known-hosts"),
		"channel trust assets were not stored");
	const auto public_json = msap1::settings::SettingsCodec::encode(
		handler.active().settings, false);
	require(!public_json.contains("secret-token"),
		"channel secret leaked into public settings");
	handler.clear_secret(prefix + "bearer-token");
	handler.delete_asset(prefix + "ca");
	require(!handler.has_secret(prefix + "bearer-token") &&
		!handler.has_asset(prefix + "ca") &&
		handler.has_asset(prefix + "known-hosts"),
		"channel material deletion changed an unrelated material");
	handler.set_secret(prefix + "bearer-token", "secret-token");
	handler.put_asset(prefix + "ca",
		"-----BEGIN CERTIFICATE-----\ntest\n-----END CERTIFICATE-----\n");
	(void)handler.factory_reset(true);
	require(!handler.has_secret(prefix + "bearer-token") &&
		!handler.has_asset(prefix + "ca"),
		"factory reset retained channel credentials");
}

void test_settings_ipc_round_trip()
{
	using namespace msap1::settings::ipc;
	Request request;
	request.command = Command::save_active;
	request.confirmed = true;
	request.json = R"({"test":true})";
	const auto frame = encode_request(request);
	const auto decoded = decode_request(frame);
	assert(decoded.command == request.command);
	assert(decoded.confirmed == request.confirmed);
	assert(decoded.json == request.json);

	Response response;
	response.status = Status::conflict;
	response.content_hash = "0123456789abcdef";
	response.message = "save conflict";
	response.json = R"({"current":true})";
	const auto response_frame = encode_response(
		response, frame.correlation_id, request.command);
	const auto decoded_response = decode_response(response_frame);
	assert(response_frame.kind == mnc::ipc::FrameKind::error);
	assert(decoded_response.status == response.status);
	assert(decoded_response.content_hash == response.content_hash);
	assert(decoded_response.message == response.message);
	assert(decoded_response.json == response.json);

	response.status = Status::ok;
	const auto event = encode_event(response);
	assert(event.kind == mnc::ipc::FrameKind::event);
	assert(event.correlation_id == 0u);
	assert(decode_response(event).content_hash == response.content_hash);
}

} // namespace

int main()
{
	test_first_boot_and_direct_save();
	test_demand_profile_validation();
	test_power_quality_settings_and_wire_snapshot();
	test_nominal_frequency_validation();
	test_system_nominal_voltage_validation();
	test_measurement_topology_validation();
	test_existing_settings_default_system_nominal_voltage();
	test_failed_apply_preserves_active();
	test_empty_active_recovers_factory_defaults();
	test_invalid_active_recovers_factory_defaults();
	test_secrets_and_factory_reset();
	test_schema_four_migrates_to_empty_data_logging();
	test_data_logging_configuration_validation();
	test_channel_scoped_credentials_and_assets();
	test_settings_ipc_round_trip();
}
