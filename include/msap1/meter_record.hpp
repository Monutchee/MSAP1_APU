#ifndef MSAP1_METER_RECORD_HPP
#define MSAP1_METER_RECORD_HPP

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <type_traits>

namespace msap1 {

inline constexpr std::size_t meter_channel_count = 8;
inline constexpr std::size_t meter_record_word_count = 64;
inline constexpr std::size_t meter_record_size = 256;
inline constexpr std::uint32_t meter_record_magic = 0x3152544du;
inline constexpr std::uint32_t meter_periodic_format = 0x00010001u;

struct MeterChannelReading {
	bool valid = false;
	std::int64_t mean_micro_units = 0;
	std::uint32_t rms_count = 0;
	std::int64_t rms_micro_units = 0;
};

struct MeterFrequencyReading {
	bool enabled = false;
	bool valid = false;
	bool reference_valid = false;
	bool out_of_range = false;
	bool timed_out = false;
	bool arithmetic_error = false;
	std::uint32_t millihz = 0;
	std::uint32_t period_q16_samples = 0;
	std::uint32_t measurement_sequence = 0;
	std::uint8_t mode = 0;
	std::uint8_t reference_channel = 0;
	std::uint8_t cycles_used = 0;
};

struct MeterRecord {
	std::array<std::uint32_t, meter_record_word_count> words{};

	std::uint32_t word(std::size_t index) const
	{
		if (index >= words.size())
			throw std::out_of_range("meter record word index");
		if constexpr (std::endian::native == std::endian::little)
			return words[index];
		return std::byteswap(words[index]);
	}

	std::uint64_t unsigned64(std::size_t low_word) const
	{
		return static_cast<std::uint64_t>(word(low_word)) |
		       (static_cast<std::uint64_t>(word(low_word + 1)) << 32);
	}

	std::int64_t signed64(std::size_t low_word) const
	{
		return std::bit_cast<std::int64_t>(unsigned64(low_word));
	}

	bool header_valid() const
	{
		return word(0) == meter_record_magic &&
		       word(1) == meter_periodic_format &&
		       word(2) == meter_record_size;
	}

	std::uint32_t sequence() const { return word(3); }
	std::uint32_t configuration_generation() const { return word(4); }
	std::uint32_t sample_rate_hz() const { return word(5); }
	std::uint32_t window_samples() const { return word(6); }
	std::uint8_t valid_mask() const { return static_cast<std::uint8_t>(word(7)); }
	std::uint32_t status() const { return word(8); }
	std::uint32_t capture_frames() const { return word(9); }
	std::uint32_t header_errors() const { return word(10); }
	std::uint32_t fifo_overflows() const { return word(11); }
	std::uint32_t packetizer_drops() const { return word(12); }
	std::uint32_t hub_drops() const { return word(13); }
	std::uint32_t adc_alerts() const { return word(14); }

	MeterChannelReading channel(std::size_t index) const
	{
		if (index >= meter_channel_count)
			throw std::out_of_range("meter channel index");
		const auto base = 16u + index * 5u;
		return {
			(valid_mask() & (1u << index)) != 0u,
			signed64(base),
			word(base + 2u),
			signed64(base + 3u),
		};
	}

	MeterFrequencyReading frequency() const
	{
		const auto frequency_status = word(57);
		return {
			(frequency_status & (1u << 0)) != 0u,
			(frequency_status & (1u << 1)) != 0u,
			(frequency_status & (1u << 2)) != 0u,
			(frequency_status & (1u << 5)) != 0u,
			(frequency_status & (1u << 6)) != 0u,
			(frequency_status & (1u << 7)) != 0u,
			word(56),
			word(58),
			word(59),
			static_cast<std::uint8_t>((frequency_status >> 8) & 0x7u),
			static_cast<std::uint8_t>((frequency_status >> 12) & 0xfu),
			static_cast<std::uint8_t>((frequency_status >> 16) & 0xffu),
		};
	}
};

static_assert(sizeof(MeterRecord) == meter_record_size,
	      "meter record must match the fixed PL DMA format");
static_assert(std::is_trivially_copyable_v<MeterRecord>);

} // namespace msap1

#endif // MSAP1_METER_RECORD_HPP
