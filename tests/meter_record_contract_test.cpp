#include "msap1/meter/meter_record.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace {

constexpr std::array<std::uint32_t, 12> public_allocations{
	msap1::meter_ten_minute_open_format,
	msap1::meter_ten_minute_open_power_format,
	msap1::meter_ten_minute_open_phasor_format,
	msap1::meter_ten_minute_open_unbalance_format,
	msap1::meter_two_hour_open_format,
	msap1::meter_two_hour_open_power_format,
	msap1::meter_two_hour_open_phasor_format,
	msap1::meter_two_hour_open_unbalance_format,
	msap1::meter_pq_event_lifecycle_format,
	msap1::meter_flicker_format,
	msap1::meter_mains_signal_format,
	msap1::meter_frequency_10s_format,
};

constexpr std::array<std::uint32_t, 8> migrated_preview_formats{
	msap1::meter_ten_minute_open_format,
	msap1::meter_ten_minute_open_power_format,
	msap1::meter_ten_minute_open_phasor_format,
	msap1::meter_ten_minute_open_unbalance_format,
	msap1::meter_two_hour_open_format,
	msap1::meter_two_hour_open_power_format,
	msap1::meter_two_hour_open_phasor_format,
	msap1::meter_two_hour_open_unbalance_format,
};

/* The old fundamental IDs are intentionally reclaimed as FLICKER/MAINS and
 * become valid only when those decoders land. The six old companion IDs have
 * no new meaning and must remain rejected. */
constexpr std::array<std::uint32_t, 6> retired_preview_formats{
	0x00190001u,
	0x001A0002u,
	0x001B0002u,
	0x001C0001u,
	0x001D0002u,
	0x001E0002u,
};

constexpr bool allocations_are_unique()
{
	for (std::size_t left = 0; left < public_allocations.size(); ++left)
		for (std::size_t right = left + 1;
		     right < public_allocations.size(); ++right)
			if (public_allocations[left] == public_allocations[right])
				return false;
	return true;
}

static_assert(msap1::meter_ten_minute_open_format == 0x00200001u);
static_assert(msap1::meter_ten_minute_open_power_format == 0x00210001u);
static_assert(msap1::meter_ten_minute_open_phasor_format == 0x00220002u);
static_assert(msap1::meter_ten_minute_open_unbalance_format == 0x00230002u);
static_assert(msap1::meter_two_hour_open_format == 0x00240001u);
static_assert(msap1::meter_two_hour_open_power_format == 0x00250001u);
static_assert(msap1::meter_two_hour_open_phasor_format == 0x00260002u);
static_assert(msap1::meter_two_hour_open_unbalance_format == 0x00270002u);
static_assert(msap1::meter_pq_event_lifecycle_format == 0x00060001u);
static_assert(msap1::meter_flicker_format == 0x000E0001u);
static_assert(msap1::meter_mains_signal_format == 0x000F0001u);
static_assert(msap1::meter_frequency_10s_format == 0x00280001u);
static_assert(msap1::meter_pq_event_lifecycle_format !=
	      msap1::meter_pq_event_format);
static_assert(allocations_are_unique());

} // namespace

int main()
{
	msap1::MeterRecord record{};
	record.words[0] = msap1::meter_record_magic;
	record.words[2] = msap1::meter_record_size;
	for (const auto format : migrated_preview_formats) {
		record.words[1] = format;
		if (!record.header_valid()) {
			std::fprintf(stderr,
				"migrated preview format 0x%08x was rejected\n",
				format);
			return 1;
		}
	}
	record.words[1] = msap1::meter_frequency_10s_format;
	if (!record.header_valid()) {
		std::fprintf(stderr, "FREQUENCY-10S-v1 format was rejected\n");
		return 1;
	}
	for (const auto format : retired_preview_formats) {
		record.words[1] = format;
		if (record.header_valid()) {
			std::fprintf(stderr,
				"retired preview format 0x%08x was accepted\n",
				format);
			return 1;
		}
	}
	return 0;
}
