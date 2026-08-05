#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mnc::settings {

enum class ApplyImpact : std::uint8_t {
	none,
	live,
	reload_service,
	restart_service,
	reboot_required,
};

struct RevisionFile {
	std::uint64_t revision = 0;
	std::string hash;
	std::filesystem::path path;
};

/** Crash-safe writer used for all authoritative settings documents. */
class AtomicFileWriter final {
public:
	static void write(const std::filesystem::path &path,
			  std::string_view contents, unsigned mode = 0640);
};

/** Owns the fixed on-disk layout below one persistent settings directory. */
class FileSettingsRepository final {
public:
	explicit FileSettingsRepository(std::filesystem::path root);

	void initialize_layout() const;
	[[nodiscard]] std::optional<std::string> read_active() const;
	[[nodiscard]] std::optional<std::string> read_draft() const;
	[[nodiscard]] std::optional<std::string> read_pending() const;
	void write_active(std::string_view json) const;
	void write_draft(std::string_view json) const;
	void write_pending(std::string_view json) const;
	void remove_draft() const;
	void remove_pending() const;

	[[nodiscard]] const std::filesystem::path &root() const noexcept;
	[[nodiscard]] std::filesystem::path active_path() const;
	[[nodiscard]] std::filesystem::path draft_path() const;
	[[nodiscard]] std::filesystem::path pending_path() const;
	[[nodiscard]] std::filesystem::path revisions_path() const;
	[[nodiscard]] std::filesystem::path secrets_path() const;

private:
	std::filesystem::path root_;
};

/** Immutable full-snapshot revision storage with bounded housekeeping. */
class RevisionStore final {
public:
	explicit RevisionStore(std::filesystem::path directory);
	RevisionFile create(std::uint64_t revision, std::string_view hash,
			    std::string_view canonical_json) const;
	RevisionFile reset(std::string_view hash,
			   std::string_view canonical_json) const;
	[[nodiscard]] std::vector<RevisionFile> list() const;
	[[nodiscard]] std::string read(std::uint64_t revision) const;
	void erase(const RevisionFile &revision) const;
	void prune(std::size_t retain) const;
	void clear() const;

private:
	std::filesystem::path directory_;
};

struct StructuredDifference {
	std::string path;
	std::string before;
	std::string after;
};

struct DiffResult {
	std::vector<StructuredDifference> changes;
	std::string unified;
};

struct SettingsPath {
	std::string value;
};

struct SettingsChangeSet {
	DiffResult diff;

	[[nodiscard]] bool affects(std::string_view prefix) const noexcept;
};

struct SettingsApplyContext {
	std::string transaction_id;
	std::string previous_json;
	std::string candidate_json;
	SettingsChangeSet changes;
};

struct ValidationResult {
	bool accepted = true;
	std::string message;
	ApplyImpact impact = ApplyImpact::none;
};

struct PrepareResult {
	bool prepared = true;
	std::string message;
};

struct ApplyResult {
	bool applied = true;
	std::string message;
};

struct HealthResult {
	bool healthy = true;
	std::string message;
};

/**
 * Trusted, compiled post-commit action. Settings documents contain policy
 * values only; they can never name commands, scripts, or systemd units.
 */
class SettingsApplier {
public:
	virtual ~SettingsApplier() = default;
	[[nodiscard]] virtual std::string_view name() const noexcept = 0;
	[[nodiscard]] virtual bool handles(const SettingsPath &path) const noexcept = 0;
	[[nodiscard]] virtual ValidationResult validate(
		const SettingsChangeSet &changes) const = 0;
	[[nodiscard]] virtual PrepareResult prepare(
		const SettingsApplyContext &context) = 0;
	[[nodiscard]] virtual ApplyResult apply(
		const SettingsApplyContext &context) = 0;
	[[nodiscard]] virtual HealthResult verify(
		const SettingsApplyContext &context) = 0;
	virtual void rollback(const SettingsApplyContext &context) noexcept = 0;
};

/** Deterministic JSON-line diff used by CLI, Web, and revision review. */
class JsonDiff final {
public:
	[[nodiscard]] static DiffResult compare(std::string_view before,
						std::string_view after);
};

/** Separate credential store. Callers never receive secret values back. */
class SecretStore final {
public:
	explicit SecretStore(std::filesystem::path path);
	void replace(std::string_view canonical_json) const;
	[[nodiscard]] std::optional<std::string> read_document() const;
	[[nodiscard]] bool exists() const;
	void clear() const;

private:
	std::filesystem::path path_;
};

} // namespace mnc::settings
