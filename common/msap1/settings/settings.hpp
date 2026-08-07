#pragma once

#include "mnc/settings/settings.hpp"
#include "msap1/settings/definition/product_settings.hpp"

#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

namespace msap1::settings {

inline constexpr std::string_view socket_path =
	"/run/monutchee/settings.sock";
inline constexpr std::string_view persistent_root = "/data/mnc/settings";
inline constexpr std::string_view factory_defaults_path =
	"/usr/share/monutchee/msap1/settings/factory-defaults.json";

struct ActiveSnapshot {
	std::string content_hash;
	ProductSettings settings;
};

class SettingsCodec final {
public:
	[[nodiscard]] static ProductSettings decode(std::string_view json);
	[[nodiscard]] static std::string encode(const ProductSettings &settings,
						  bool pretty = true);
	[[nodiscard]] static std::string hash(std::string_view canonical_json);
};

class SettingsValidator final {
public:
	static void validate(const ProductSettings &settings);
};

/** Coordinates runtime apply/verify/rollback without knowing device details. */
class SettingsApplyCoordinator final {
public:
	using Apply = std::function<void(const ProductSettings &)>;
	SettingsApplyCoordinator(Apply apply = {});
	void apply(const ProductSettings &candidate,
		   const ProductSettings &previous) const;
	void rollback(const ProductSettings &previous) const noexcept;

private:
	Apply apply_;
};

/** Single-writer typed settings aggregate. */
class SettingsHandler final {
public:
	SettingsHandler(std::filesystem::path root,
			std::filesystem::path factory_defaults,
			SettingsApplyCoordinator coordinator = {});
	~SettingsHandler();
	SettingsHandler(const SettingsHandler &) = delete;
	SettingsHandler &operator=(const SettingsHandler &) = delete;

	void initialize();
	[[nodiscard]] ActiveSnapshot active() const;
	/** Validate, hot-apply, and atomically persist a complete settings document. */
	[[nodiscard]] ActiveSnapshot save(const ProductSettings &settings);
	[[nodiscard]] ActiveSnapshot factory_reset(bool confirmed);
	void set_secret_document(std::string_view canonical_json);
	[[nodiscard]] bool has_secrets() const;
	[[nodiscard]] bool recovery_mode() const;
	[[nodiscard]] std::string recovery_reason() const;

private:
	void bootstrap_locked();
	[[nodiscard]] ProductSettings load_factory_locked() const;
	[[nodiscard]] ActiveSnapshot save_locked(const ProductSettings &settings,
		bool allow_recovery);

	mutable std::mutex mutex_;
	mnc::settings::FileSettingsRepository repository_;
	mnc::settings::SecretStore secrets_;
	std::filesystem::path factory_defaults_;
	SettingsApplyCoordinator coordinator_;
	ActiveSnapshot active_;
	bool recovery_mode_ = false;
	std::string recovery_reason_;
	int lock_fd_ = -1;
};

} // namespace msap1::settings
