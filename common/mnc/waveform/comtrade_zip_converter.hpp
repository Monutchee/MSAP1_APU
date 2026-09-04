#pragma once

#include "mnc/waveform/waveform_converter.hpp"

namespace mnc::waveform {

/** IEC 60255-24:2013 legacy CFG/DAT pair in a bounded streaming ZIP. */
class ComtradeZipConverter final : public WaveformConverter {
public:
	[[nodiscard]] ConversionSummary convert(const WaveformSource &source,
		OutputSink &sink, const ConversionOptions &options,
		std::stop_token stop_token = {},
		ProgressCallback progress = {}) const override;
};

} // namespace mnc::waveform
