#pragma once

#include "msap1/settings.hpp"
#include "msap1/settings_ipc.hpp"

#include <functional>
#include <string>
#include <string_view>

namespace msap1::web {

/** Typed application boundary between HTTP controllers and settings IPC. */
class SettingsGateway final {
public:
	using Mutator = std::function<void(settings::ProductSettings &)>;

	[[nodiscard]] settings::ipc::Response active(int timeout_ms = 3000) const;
	[[nodiscard]] settings::ipc::Response draft(int timeout_ms = 3000) const;
	[[nodiscard]] settings::ipc::Response diff(int timeout_ms = 3000) const;
	[[nodiscard]] settings::ipc::Response history(int timeout_ms = 3000) const;
	[[nodiscard]] settings::ipc::Response revision(
		std::uint64_t revision, int timeout_ms = 3000) const;
	[[nodiscard]] settings::ipc::Response patch(
		const settings::ProductSettings &value,
		std::uint64_t expected_generation, int timeout_ms = 5000) const;
	[[nodiscard]] settings::ipc::Response commit(
		std::string_view message, std::uint64_t expected_revision,
		std::uint64_t expected_generation, int timeout_ms = 35000) const;
	[[nodiscard]] settings::ipc::Response discard(int timeout_ms = 3000) const;
	[[nodiscard]] settings::ipc::Response restore(
		std::uint64_t revision, int timeout_ms = 3000) const;
	[[nodiscard]] settings::ipc::Response factory_reset(
		bool confirmed, int timeout_ms = 35000) const;

	/** Update one typed section and commit it through the common transaction. */
	[[nodiscard]] settings::ProductSettings update_and_commit(
		const Mutator &mutator, std::string_view message,
		int timeout_ms = 35000) const;

private:
	[[nodiscard]] settings::ipc::Response require_ok(
		settings::ipc::Request request, int timeout_ms) const;

	mutable settings::ipc::SettingsClient client_;
};

} // namespace msap1::web
