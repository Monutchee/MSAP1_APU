#include "settings_gateway.hpp"

#include <stdexcept>
#include <utility>

namespace msap1::web {

settings::ipc::Response SettingsGateway::require_ok(
	settings::ipc::Request request, int timeout_ms) const
{
	auto response = client_.request(std::move(request), timeout_ms);
	if (response.status != settings::ipc::Status::ok)
		throw std::runtime_error(response.message.empty()
			? "settings service rejected the request" : response.message);
	return response;
}

settings::ipc::Response SettingsGateway::active(int timeout_ms) const
{
	settings::ipc::Request request;
	request.command = settings::ipc::Command::get_active;
	/* Recovery mode intentionally permits read-only diagnostics.  Mutation
	 * methods still go through require_ok and therefore remain blocked until an
	 * explicit factory reset repairs the authority. */
	auto response = client_.request(std::move(request), timeout_ms);
	if (response.status != settings::ipc::Status::ok &&
	    response.status != settings::ipc::Status::recovery_mode)
		throw std::runtime_error(response.message.empty()
			? "settings service rejected the active settings request"
			: response.message);
	return response;
}

settings::ipc::Response SettingsGateway::draft(int timeout_ms) const
{
	settings::ipc::Request request;
	request.command = settings::ipc::Command::get_draft;
	return require_ok(std::move(request), timeout_ms);
}

settings::ipc::Response SettingsGateway::diff(int timeout_ms) const
{
	settings::ipc::Request request;
	request.command = settings::ipc::Command::get_diff;
	return require_ok(std::move(request), timeout_ms);
}

settings::ipc::Response SettingsGateway::history(int timeout_ms) const
{
	settings::ipc::Request request;
	request.command = settings::ipc::Command::list_revisions;
	return require_ok(std::move(request), timeout_ms);
}

settings::ipc::Response SettingsGateway::revision(
	std::uint64_t number, int timeout_ms) const
{
	settings::ipc::Request request;
	request.command = settings::ipc::Command::get_revision;
	request.revision = number;
	return require_ok(std::move(request), timeout_ms);
}

settings::ipc::Response SettingsGateway::patch(
	const settings::ProductSettings &value,
	std::uint64_t expected_generation, int timeout_ms) const
{
	settings::ipc::Request request;
	request.command = settings::ipc::Command::patch_draft;
	request.expected_generation = expected_generation;
	request.json = settings::SettingsCodec::encode(value, false);
	return require_ok(std::move(request), timeout_ms);
}

settings::ipc::Response SettingsGateway::commit(
	std::string_view message, std::uint64_t expected_revision,
	std::uint64_t expected_generation, int timeout_ms) const
{
	settings::ipc::Request request;
	request.command = settings::ipc::Command::commit_draft;
	request.message = std::string(message);
	request.expected_revision = expected_revision;
	request.expected_generation = expected_generation;
	return require_ok(std::move(request), timeout_ms);
}

settings::ipc::Response SettingsGateway::discard(int timeout_ms) const
{
	settings::ipc::Request request;
	request.command = settings::ipc::Command::discard_draft;
	return require_ok(std::move(request), timeout_ms);
}

settings::ipc::Response SettingsGateway::restore(
	std::uint64_t number, int timeout_ms) const
{
	settings::ipc::Request request;
	request.command = settings::ipc::Command::restore_to_draft;
	request.revision = number;
	return require_ok(std::move(request), timeout_ms);
}

settings::ipc::Response SettingsGateway::factory_reset(
	bool confirmed, int timeout_ms) const
{
	settings::ipc::Request request;
	request.command = settings::ipc::Command::factory_reset;
	request.confirmed = confirmed;
	return require_ok(std::move(request), timeout_ms);
}

settings::ProductSettings SettingsGateway::update_and_commit(
	const Mutator &mutator, std::string_view message, int timeout_ms) const
{
	/* Compatibility routes are immediate-commit adapters.  Never fold an
	 * unrelated browser draft into one of these updates; the staged settings
	 * UI must explicitly commit or discard its draft first. */
	const auto pending = diff(timeout_ms);
	if (!pending.message.empty())
		throw std::runtime_error(
			"settings draft has unsaved changes; commit or discard it first");
	const auto current = draft();
	auto value = settings::SettingsCodec::decode(current.json);
	mutator(value);
	const auto candidate = patch(value, current.generation);
	(void)commit(message, candidate.revision, candidate.generation, timeout_ms);
	return value;
}

} // namespace msap1::web
