#include "mnc/settings/settings.hpp"
#include "msap1/settings.hpp"
#include "msap1/settings_ipc.hpp"

#include <glaze/glaze.hpp>

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

struct PendingFixture {
	std::uint64_t previous_revision = 0;
	std::uint64_t draft_generation = 0;
	std::string transaction_id;
	std::string message;
	ProductSettings previous;
	ProductSettings candidate;
};

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
		const auto source = std::filesystem::path(PROJECT_SOURCE_DIR) /
			"config/settings/factory-defaults.json";
		std::filesystem::copy_file(source, factory,
			std::filesystem::copy_options::overwrite_existing);
	}

	~TestTree()
	{
		std::error_code ignored;
		std::filesystem::remove_all(root, ignored);
	}
};

void patch_and_commit(SettingsHandler &handler, ProductSettings settings,
		      std::string_view message)
{
	const auto draft = handler.draft();
	const auto patched = handler.patch({std::move(settings)}, draft.generation);
	const auto transaction = handler.commit(message, patched.draft.base_revision,
		patched.draft.generation, {0u, "test"});
	assert(transaction.committed);
}

void test_first_boot_diff_and_stale_edit()
{
	TestTree tree("first-boot");
	SettingsHandler handler(tree.data, tree.factory);
	handler.initialize();
	assert(handler.active().revision == 1u);
	assert(handler.active().settings.metering.sample_rate_hz == 32000u);
	assert(handler.history().size() == 1u);

	auto draft = handler.draft();
	draft.settings.waveform.default_pretrigger_ms = 2500u;
	const auto patched = handler.patch({draft.settings}, draft.generation);
	assert(patched.draft.generation == 1u);
	const auto difference = handler.diff();
	assert(!difference.changes.empty());
	assert(difference.changes.front().path ==
		"$.waveform.default_pretrigger_ms");
	assert(difference.unified.find("--- active.json") != std::string::npos);

	bool stale_rejected = false;
	try {
		(void)handler.patch({draft.settings}, 0u);
	} catch (const std::runtime_error &) {
		stale_rejected = true;
	}
	assert(stale_rejected);

	const auto committed = handler.commit("test", 1u, 1u, {0u, "test"});
	assert(committed.committed);
	assert(handler.active().revision == 2u);
	assert(handler.active().settings.waveform.default_pretrigger_ms == 2500u);
	assert(!std::filesystem::exists(tree.data / "draft.json"));
	assert(!std::filesystem::exists(tree.data / "pending.json"));
}

void test_revision_retention_and_restore()
{
	TestTree tree("retention");
	SettingsHandler handler(tree.data, tree.factory);
	handler.initialize();

	auto settings = handler.active().settings;
	settings.system.retained_revisions = 2u;
	settings.waveform.default_pretrigger_ms = 1001u;
	patch_and_commit(handler, settings, "retain two");
	settings = handler.active().settings;
	settings.waveform.default_pretrigger_ms = 1002u;
	patch_and_commit(handler, settings, "revision three");
	settings = handler.active().settings;
	settings.waveform.default_pretrigger_ms = 1003u;
	patch_and_commit(handler, settings, "revision four");

	const auto history = handler.history();
	assert(history.size() == 2u);
	assert(history.front().revision == 3u);
	assert(history.back().revision == 4u);
	const auto restored = handler.restore_to_draft(3u);
	assert(restored.base_revision == 4u);
	assert(restored.settings.waveform.default_pretrigger_ms == 1002u);
	handler.discard();
	assert(handler.diff().changes.empty());
}

void test_failed_apply_rolls_back_and_preserves_draft()
{
	TestTree tree("rollback");
	std::uint32_t applied_pretrigger = 10000u;
	msap1::settings::SettingsApplyCoordinator coordinator{
		[&](const ProductSettings &settings) {
			if (settings.waveform.default_pretrigger_ms == 7777u)
				throw std::runtime_error("injected apply failure");
			applied_pretrigger = settings.waveform.default_pretrigger_ms;
		}};
	SettingsHandler handler(tree.data, tree.factory, std::move(coordinator));
	handler.initialize();
	auto settings = handler.active().settings;
	settings.waveform.default_pretrigger_ms = 7777u;
	const auto draft = handler.patch({settings}, 0u).draft;
	bool failed = false;
	try {
		(void)handler.commit("must fail", draft.base_revision,
			draft.generation, {0u, "test"});
	} catch (const std::runtime_error &) {
		failed = true;
	}
	assert(failed);
	assert(handler.active().revision == 1u);
	assert(handler.draft().settings.waveform.default_pretrigger_ms == 7777u);
	assert(applied_pretrigger == 10000u);
	assert(!std::filesystem::exists(tree.data / "pending.json"));
}

void test_secrets_and_factory_reset()
{
	TestTree tree("factory-reset");
	SettingsHandler handler(tree.data, tree.factory);
	handler.initialize();
	handler.set_secret_document(R"({"mqtt_password":"not-public"})");
	assert(handler.has_secrets());
	struct stat status {};
	assert(::stat((tree.data / "secrets.json").c_str(), &status) == 0);
	assert((status.st_mode & 0777u) == 0600u);

	auto settings = handler.active().settings;
	settings.waveform.default_posttrigger_ms = 1234u;
	patch_and_commit(handler, settings, "custom setting");
	const auto reset = handler.factory_reset(
		{true, {0u, "test administrator"}});
	assert(reset.committed);
	assert(reset.revision == 1u);
	assert(handler.active().revision == 1u);
	assert(handler.active().settings.waveform.default_posttrigger_ms == 10000u);
	assert(handler.history().size() == 1u);
	assert(!handler.has_secrets());
}

void test_corrupt_active_enters_recovery()
{
	TestTree tree("recovery");
	{
		SettingsHandler handler(tree.data, tree.factory);
		handler.initialize();
	}
	{
		std::ofstream output(tree.data / "active.json", std::ios::trunc);
		output << "{not-valid-json";
	}
	SettingsHandler recovered(tree.data, tree.factory);
	recovered.initialize();
	assert(recovered.recovery_mode());
	assert(!recovered.recovery_reason().empty());
	bool mutation_rejected = false;
	try {
		(void)recovered.patch({recovered.active().settings}, 0u);
	} catch (const std::runtime_error &) {
		mutation_rejected = true;
	}
	assert(mutation_rejected);
	const auto reset = recovered.factory_reset({true, {0u, "recovery test"}});
	assert(reset.committed);
	assert(!recovered.recovery_mode());
	assert(recovered.active().revision == 1u);
}

void test_empty_active_reloads_factory_defaults()
{
	TestTree tree("empty-active");
	{
		SettingsHandler handler(tree.data, tree.factory);
		handler.initialize();
		auto settings = handler.active().settings;
		settings.waveform.default_pretrigger_ms = 4321u;
		patch_and_commit(handler, settings, "custom setting");
	}
	{
		std::ofstream output(tree.data / "active.json", std::ios::trunc);
	}

	SettingsHandler recovered(tree.data, tree.factory);
	recovered.initialize();
	assert(!recovered.recovery_mode());
	assert(recovered.active().revision == 1u);
	assert(recovered.active().settings.waveform.default_pretrigger_ms == 10000u);
	assert(recovered.history().size() == 1u);
}

void test_factory_document_is_required_and_valid()
{
	{
		TestTree tree("missing-factory");
		std::filesystem::remove(tree.factory);
		bool rejected = false;
		try {
			SettingsHandler handler(tree.data, tree.factory);
			handler.initialize();
		} catch (const std::runtime_error &) {
			rejected = true;
		}
		assert(rejected);
		assert(!std::filesystem::exists(tree.data / "active.json"));
	}
	{
		TestTree tree("invalid-factory");
		std::ofstream output(tree.factory, std::ios::trunc);
		output << R"({"schema_version":1})";
		output.close();
		bool rejected = false;
		try {
			SettingsHandler handler(tree.data, tree.factory);
			handler.initialize();
		} catch (const std::runtime_error &) {
			rejected = true;
		}
		assert(rejected);
		assert(!std::filesystem::exists(tree.data / "active.json"));
	}
}

void test_pending_transaction_restores_active_and_preserves_candidate()
{
	TestTree tree("pending-recovery");
	ProductSettings previous;
	ProductSettings candidate;
	{
		SettingsHandler handler(tree.data, tree.factory);
		handler.initialize();
		previous = handler.active().settings;
		candidate = previous;
		candidate.waveform.default_posttrigger_ms = 4321u;
	}

	{
		std::ofstream active(tree.data / "active.json", std::ios::trunc);
		active << msap1::settings::SettingsCodec::encode(candidate);
	}
	const PendingFixture pending{1u, 7u, "settings-test-pending",
		"interrupted test transaction", previous, candidate};
	const auto pending_json = glz::write<glz::opts{.prettify = true}>(pending);
	assert(pending_json);
	{
		std::ofstream output(tree.data / "pending.json", std::ios::trunc);
		output << *pending_json << '\n';
	}

	std::uint32_t restored_posttrigger = 0u;
	msap1::settings::SettingsApplyCoordinator coordinator{
		[&](const ProductSettings &settings) {
			restored_posttrigger =
				settings.waveform.default_posttrigger_ms;
		}};
	SettingsHandler recovered(tree.data, tree.factory, std::move(coordinator));
	recovered.initialize();
	assert(!recovered.recovery_mode());
	assert(recovered.active().revision == 1u);
	assert(recovered.active().settings.waveform.default_posttrigger_ms ==
		previous.waveform.default_posttrigger_ms);
	assert(recovered.draft().generation == 7u);
	assert(recovered.draft().settings.waveform.default_posttrigger_ms == 4321u);
	assert(restored_posttrigger == previous.waveform.default_posttrigger_ms);
	assert(!std::filesystem::exists(tree.data / "pending.json"));
}

void test_settings_ipc_round_trip()
{
	using namespace msap1::settings::ipc;
	Request request;
	request.command = Command::commit_draft;
	request.expected_revision = 14u;
	request.expected_generation = 9u;
	request.revision = 3u;
	request.confirmed = true;
	request.message = "operator commit";
	request.json = R"({"test":true})";
	const auto frame = encode_request(request);
	const auto decoded = decode_request(frame);
	assert(decoded.command == request.command);
	assert(decoded.expected_revision == request.expected_revision);
	assert(decoded.expected_generation == request.expected_generation);
	assert(decoded.revision == request.revision);
	assert(decoded.confirmed == request.confirmed);
	assert(decoded.message == request.message);
	assert(decoded.json == request.json);

	Response response;
	response.status = Status::conflict;
	response.revision = 15u;
	response.generation = 10u;
	response.transaction_id = "settings-15-test";
	response.message = "stale draft";
	response.json = R"({"current":15})";
	const auto response_frame = encode_response(response, frame.correlation_id,
		request.command);
	const auto decoded_response = decode_response(response_frame);
	assert(response_frame.kind == mnc::ipc::FrameKind::error);
	assert(decoded_response.status == response.status);
	assert(decoded_response.revision == response.revision);
	assert(decoded_response.generation == response.generation);
	assert(decoded_response.transaction_id == response.transaction_id);
	assert(decoded_response.message == response.message);
	assert(decoded_response.json == response.json);

	response.status = Status::ok;
	const auto event = encode_event(response);
	assert(event.kind == mnc::ipc::FrameKind::event);
	assert(event.correlation_id == 0u);
	assert(decode_response(event).transaction_id == response.transaction_id);
}

} // namespace

int main()
{
	test_first_boot_diff_and_stale_edit();
	test_revision_retention_and_restore();
	test_failed_apply_rolls_back_and_preserves_draft();
	test_secrets_and_factory_reset();
	test_corrupt_active_enters_recovery();
	test_empty_active_reloads_factory_defaults();
	test_factory_document_is_required_and_valid();
	test_pending_transaction_restores_active_and_preserves_candidate();
	test_settings_ipc_round_trip();
}
