#pragma once

#include "mnc/waveform/waveform_converter.hpp"
#include "msap1/waveform/mncwf_v4.hpp"

#include <memory>
#include <optional>

namespace msap1 {

/**
 * Bounded adapter from one retained, completed MNCWF v4/v5 descriptor to the
 * product-neutral conversion source. The descriptor is duplicated on entry,
 * so unlinking or replacing the pathname cannot alter an accepted job.
 */
class MncwfWaveformSource final : public mnc::waveform::WaveformSource {
public:
	MncwfWaveformSource(int descriptor, mnc::waveform::ExportFormat format,
		mnc::waveform::ExportScope scope =
			mnc::waveform::ExportScope::capture,
		std::optional<MncwfUuid> event_uuid = std::nullopt);
	~MncwfWaveformSource() override;
	MncwfWaveformSource(const MncwfWaveformSource &) = delete;
	MncwfWaveformSource &operator=(const MncwfWaveformSource &) = delete;

	[[nodiscard]] const mnc::waveform::WaveformMetadata &metadata()
		const noexcept override;
	[[nodiscard]] std::uint64_t frame_count() const noexcept override;
	[[nodiscard]] std::size_t channel_count() const noexcept override;
	[[nodiscard]] std::size_t read_frames(std::uint64_t first_frame,
		std::size_t frame_capacity,
		std::span<std::int64_t> destination) const override;
	[[nodiscard]] std::uint32_t source_version() const noexcept;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace msap1
