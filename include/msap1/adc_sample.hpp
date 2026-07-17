#ifndef MSAP1_ADC_SAMPLE_HPP
#define MSAP1_ADC_SAMPLE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace msap1 {

inline constexpr std::size_t adc_channel_count = 8;
inline constexpr std::uint32_t adc_default_sample_rate_hz = 32000;
inline constexpr std::uint32_t adc_frames_per_pl_packet = 256;

using AdcSampleFrame = std::array<std::int32_t, adc_channel_count>;
static_assert(sizeof(AdcSampleFrame) == 32,
	      "AD7771 frame must contain eight 32-bit storage words");

struct AdcBatch {
	std::uint32_t adc_sample_rate_hz = adc_default_sample_rate_hz;
	std::uint32_t display_rate_hz = adc_default_sample_rate_hz;
	std::uint64_t first_frame_index = 0;
	std::uint32_t capture_flags = 0;
	std::vector<AdcSampleFrame> frames;
};

} // namespace msap1

#endif
