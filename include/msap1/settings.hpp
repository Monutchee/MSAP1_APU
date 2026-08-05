#pragma once

#include "mnc/settings/settings.hpp"
#include "msap1/meter_config.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace msap1::settings {

inline constexpr std::string_view socket_path =
	"/run/monutchee/settings.sock";
inline constexpr std::string_view persistent_root = "/data/mnc/settings";
inline constexpr std::string_view factory_defaults_path =
	"/usr/share/monutchee/msap1/settings/factory-defaults.json";

struct SystemSettings {
	std::uint32_t retained_revisions = 0;
};

struct RmsSettings {
	std::uint32_t window_ms = 0;
	bool remove_dc = false;
};

struct MeterConversionSettings {
	std::string profile_id;
	double adc_reference_volts = 0.0;
	std::vector<CurrentChannelConfig> current_channels;
	std::vector<VoltageChannelConfig> voltage_channels;
};

struct MeteringSettings {
	std::uint32_t sample_rate_hz = 0;
	RmsSettings rms;
	FrequencyConfig frequency;
	MeterConversionSettings conversion;
};

struct AdcSettings {
	std::string source;
	SimulatorConfig simulator;
};

struct WaveformSettings {
	std::uint32_t default_pretrigger_ms = 0;
	std::uint32_t default_posttrigger_ms = 0;
};

struct ProductSettings {
	std::uint32_t schema_version = 1;
	SystemSettings system;
	MeteringSettings metering;
	AdcSettings adc;
	WaveformSettings waveform;
};

struct ActiveSnapshot {
	std::uint64_t revision = 0;
	std::string content_hash;
	ProductSettings settings;
};

struct DraftSnapshot {
	std::uint64_t base_revision = 0;
	std::uint64_t generation = 0;
	ProductSettings settings;
};

struct RevisionInfo {
	std::uint64_t revision = 0;
	std::string hash;
};

struct SettingsPatch {
	ProductSettings settings;
};

struct PatchResult {
	DraftSnapshot draft;
};

struct CommitActor {
	std::uint32_t uid = 0;
	std::string name;
};

struct CommitTransaction {
	std::string id;
	bool committed = false;
	std::uint64_t revision = 0;
	std::string message;
};

struct FactoryResetConfirmation {
	bool confirmed = false;
	CommitActor actor;
};

using SettingsDiff = mnc::settings::DiffResult;

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

class SettingsMigrator final {
public:
	[[nodiscard]] static ProductSettings current(ProductSettings settings);
};

[[nodiscard]] MeterConversionFile
to_meter_configuration(const ProductSettings &settings);

/** Applies metering and ADC changes through the coordinated acquisition path. */
class AcquisitionSettingsApplier final : public mnc::settings::SettingsApplier {
public:
	using Apply = std::function<void(const ProductSettings &)>;
	explicit AcquisitionSettingsApplier(Apply apply);
	[[nodiscard]] std::string_view name() const noexcept override;
	[[nodiscard]] bool handles(
		const mnc::settings::SettingsPath &path) const noexcept override;
	[[nodiscard]] mnc::settings::ValidationResult validate(
		const mnc::settings::SettingsChangeSet &changes) const override;
	[[nodiscard]] mnc::settings::PrepareResult prepare(
		const mnc::settings::SettingsApplyContext &context) override;
	[[nodiscard]] mnc::settings::ApplyResult apply(
		const mnc::settings::SettingsApplyContext &context) override;
	[[nodiscard]] mnc::settings::HealthResult verify(
		const mnc::settings::SettingsApplyContext &context) override;
	void rollback(const mnc::settings::SettingsApplyContext &context) noexcept override;

private:
	Apply apply_;
};

/** Live-applies waveform trigger defaults without restarting either DMA. */
class WaveformSettingsApplier final : public mnc::settings::SettingsApplier {
public:
	using Apply = std::function<void(const ProductSettings &)>;
	explicit WaveformSettingsApplier(Apply apply);
	[[nodiscard]] std::string_view name() const noexcept override;
	[[nodiscard]] bool handles(
		const mnc::settings::SettingsPath &path) const noexcept override;
	[[nodiscard]] mnc::settings::ValidationResult validate(
		const mnc::settings::SettingsChangeSet &changes) const override;
	[[nodiscard]] mnc::settings::PrepareResult prepare(
		const mnc::settings::SettingsApplyContext &context) override;
	[[nodiscard]] mnc::settings::ApplyResult apply(
		const mnc::settings::SettingsApplyContext &context) override;
	[[nodiscard]] mnc::settings::HealthResult verify(
		const mnc::settings::SettingsApplyContext &context) override;
	void rollback(const mnc::settings::SettingsApplyContext &context) noexcept override;

private:
	Apply apply_;
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
	[[nodiscard]] DraftSnapshot draft() const;
	PatchResult patch(const SettingsPatch &patch,
			  std::uint64_t expected_draft_generation);
	[[nodiscard]] SettingsDiff diff() const;
	CommitTransaction commit(std::string_view message,
				 std::uint64_t expected_base_revision,
				 std::uint64_t expected_draft_generation,
				 const CommitActor &actor);
	void discard();
	[[nodiscard]] std::vector<RevisionInfo> history() const;
	[[nodiscard]] ProductSettings revision(std::uint64_t revision) const;
	DraftSnapshot restore_to_draft(std::uint64_t revision);
	CommitTransaction factory_reset(
		const FactoryResetConfirmation &confirmation);
	void set_secret_document(std::string_view canonical_json);
	[[nodiscard]] bool has_secrets() const;
	[[nodiscard]] bool recovery_mode() const;
	[[nodiscard]] std::string recovery_reason() const;

private:
	void bootstrap_locked();
	void recover_pending_locked();
	[[nodiscard]] ProductSettings load_factory_locked() const;
	[[nodiscard]] std::uint64_t next_revision_locked() const;
	[[nodiscard]] DraftSnapshot draft_locked() const;
	[[nodiscard]] CommitTransaction commit_locked(
		std::string_view message, std::uint64_t expected_base_revision,
		std::uint64_t expected_draft_generation, const CommitActor &actor,
		bool reset_history);

	mutable std::mutex mutex_;
	mnc::settings::FileSettingsRepository repository_;
	mnc::settings::RevisionStore revisions_;
	mnc::settings::SecretStore secrets_;
	std::filesystem::path factory_defaults_;
	SettingsApplyCoordinator coordinator_;
	ActiveSnapshot active_;
	std::optional<DraftSnapshot> draft_;
	bool recovery_mode_ = false;
	std::string recovery_reason_;
	int lock_fd_ = -1;
};

} // namespace msap1::settings
