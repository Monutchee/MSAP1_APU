#include "msap1/settings/settings.hpp"
#include "msap1/settings/settings_ipc.hpp"

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
		assert(initial.settings.metering.sample_rate_hz == 32000u);
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

void test_failed_apply_preserves_active()
{
	TestTree tree("rollback");
	std::uint32_t runtime_pretrigger = 10000u;
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
	assert(handler.active().settings.waveform.default_pretrigger_ms == 10000u);
	assert(runtime_pretrigger == 10000u);
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
	assert(recovered.active().settings.waveform.default_pretrigger_ms == 10000u);
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
			10000u);
	}

	SettingsHandler reloaded(tree.data, tree.factory);
	reloaded.initialize();
	assert(!reloaded.recovery_mode());
	assert(reloaded.active().settings.waveform.default_posttrigger_ms == 10000u);
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
	assert(handler.active().settings.waveform.default_posttrigger_ms == 10000u);
	assert(!handler.has_secrets());
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
	test_nominal_frequency_validation();
	test_failed_apply_preserves_active();
	test_empty_active_recovers_factory_defaults();
	test_invalid_active_recovers_factory_defaults();
	test_secrets_and_factory_reset();
	test_settings_ipc_round_trip();
}
