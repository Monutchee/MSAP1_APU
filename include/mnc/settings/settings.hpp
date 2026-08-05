#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace mnc::settings {

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
	void write_active(std::string_view json) const;

	[[nodiscard]] const std::filesystem::path &root() const noexcept;
	[[nodiscard]] std::filesystem::path active_path() const;
	[[nodiscard]] std::filesystem::path secrets_path() const;

private:
	std::filesystem::path root_;
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
