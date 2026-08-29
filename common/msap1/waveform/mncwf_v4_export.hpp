#pragma once

#include "msap1/waveform/mncwf_v4.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>

namespace msap1 {

/**
 * Validated, read-only virtual event export backed by one mapped MNCWF v4
 * master. The mapping owns the parent sample bytes for the full lifetime of
 * the virtual file; only regenerated metadata is allocated.
 */
class MncwfV4ExportFile final {
public:
	[[nodiscard]] static std::shared_ptr<MncwfV4ExportFile> open(
		const std::filesystem::path &directory, std::string_view file_name,
		const MncwfUuid &event_uuid);
	~MncwfV4ExportFile();

	MncwfV4ExportFile(const MncwfV4ExportFile &) = delete;
	MncwfV4ExportFile &operator=(const MncwfV4ExportFile &) = delete;
	MncwfV4ExportFile(MncwfV4ExportFile &&) = delete;
	MncwfV4ExportFile &operator=(MncwfV4ExportFile &&) = delete;

	[[nodiscard]] std::uint64_t size() const noexcept;
	[[nodiscard]] const MncwfUuid &capture_uuid() const noexcept;
	[[nodiscard]] std::uint64_t first_sequence() const noexcept;
	[[nodiscard]] std::uint64_t last_sequence() const noexcept;
	[[nodiscard]] std::size_t read(std::uint64_t offset,
		std::span<std::byte> destination) const noexcept;

private:
	struct Impl;
	explicit MncwfV4ExportFile(std::unique_ptr<Impl> implementation);
	std::unique_ptr<Impl> impl_;
};

} // namespace msap1
