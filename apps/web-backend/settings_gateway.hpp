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
	[[nodiscard]] settings::ipc::Response save(
		const settings::ProductSettings &value, int timeout_ms = 35000) const;
	[[nodiscard]] settings::ipc::Response factory_reset(
		bool confirmed, int timeout_ms = 35000) const;

	/** Update one typed section, hot-apply it, and persist active.json. */
	[[nodiscard]] settings::ProductSettings update_and_save(
		const Mutator &mutator,
		int timeout_ms = 35000) const;

private:
	[[nodiscard]] settings::ipc::Response require_ok(
		settings::ipc::Request request, int timeout_ms) const;

	mutable settings::ipc::SettingsClient client_;
};

} // namespace msap1::web
