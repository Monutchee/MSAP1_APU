#pragma once

#include "mnc/waveform/waveform_converter.hpp"

namespace mnc::waveform {

/** IEEE 1159.3-2025 physical/logical PQDIF writer. */
class PqdifConverter final : public WaveformConverter {
public:
	[[nodiscard]] ConversionSummary convert(const WaveformSource &source,
		OutputSink &sink, const ConversionOptions &options,
		std::stop_token stop_token = {},
		ProgressCallback progress = {}) const override;
};

} // namespace mnc::waveform
