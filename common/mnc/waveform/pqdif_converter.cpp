#include "mnc/waveform/pqdif_converter.hpp"

#include <boost/multiprecision/cpp_int.hpp>
#include <boost/multiprecision/cpp_dec_float.hpp>

#include <zlib.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iomanip>
#include <limits>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace mnc::waveform {
namespace {

/*
 * The standard tag and semantic identifiers below are transcribed from the
 * IEEE PQDIF normative definitions, release 1.0.0 (Apache-2.0). Keeping the
 * small used subset here avoids importing its C ABI, whose historical LONG
 * typedef is not portable to LP64 targets.
 */

constexpr Uuid guid(std::uint32_t d1, std::uint16_t d2, std::uint16_t d3,
	std::array<std::uint8_t, 8> d4)
{
	Uuid result{};
	result[0] = static_cast<std::byte>(d1 >> 24u);
	result[1] = static_cast<std::byte>(d1 >> 16u);
	result[2] = static_cast<std::byte>(d1 >> 8u);
	result[3] = static_cast<std::byte>(d1);
	result[4] = static_cast<std::byte>(d2 >> 8u);
	result[5] = static_cast<std::byte>(d2);
	result[6] = static_cast<std::byte>(d3 >> 8u);
	result[7] = static_cast<std::byte>(d3);
	for (std::size_t index = 0; index != d4.size(); ++index)
		result[index + 8u] = static_cast<std::byte>(d4[index]);
	return result;
}

#define PQGUID(a, b, c, ...) guid(a, b, c, std::array<std::uint8_t, 8>{__VA_ARGS__})

constexpr auto record_signature = PQGUID(0x4a111440, 0xe49f, 0x11cf,
	0x99, 0x00, 0x50, 0x51, 0x44, 0x49, 0x46, 0x00);
constexpr auto rec_container = PQGUID(0x89738606, 0xf1c3, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto rec_data_source = PQGUID(0x89738619, 0xf1c3, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto rec_settings = PQGUID(0xb48d858c, 0xf5f5, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto rec_observation = PQGUID(0x8973861a, 0xf1c3, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);

constexpr auto tag_version = PQGUID(0x89738607, 0xf1c3, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_file_name = PQGUID(0x89738608, 0xf1c3, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_creation = PQGUID(0x89738609, 0xf1c3, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_comments = PQGUID(0x89738611, 0xf1c3, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_compression_style = PQGUID(0x8973861b, 0xf1c3, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_compression_algorithm = PQGUID(0x8973861c, 0xf1c3, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);

constexpr auto tag_data_source_type = PQGUID(0xb48d8581, 0xf5f5, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_serial = PQGUID(0xb48d8585, 0xf5f5, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_source_version = PQGUID(0xb48d8586, 0xf5f5, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_source_name = PQGUID(0xb48d8587, 0xf5f5, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_location = PQGUID(0xb48d8589, 0xf5f5, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_time_zone = PQGUID(0xb48d858a, 0xf5f5, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_utc_to_lst = PQGUID(0x6acec12e, 0x43b1, 0x4336,
	0xaf, 0x8f, 0x1d, 0xd2, 0x89, 0xe4, 0xe1, 0x68);
constexpr auto tag_channel_definitions = PQGUID(0xb48d858d, 0xf5f5, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_one_channel_definition = PQGUID(0xb48d858e, 0xf5f5, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_channel_name = PQGUID(0xb48d8590, 0xf5f5, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_phase = PQGUID(0xb48d8591, 0xf5f5, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_quantity_type = PQGUID(0xb48d8592, 0xf5f5, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_other_channel_id = PQGUID(0xb48d8593, 0xf5f5, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_group_name = PQGUID(0xb48d8594, 0xf5f5, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_quantity_measured = PQGUID(0xc690e872, 0xf755, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_physical_channel = PQGUID(0x89738622, 0xf1c3, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_primary_series = PQGUID(0xb48d8596, 0xf5f5, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_series_definitions = PQGUID(0xb48d8598, 0xf5f5, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_one_series_definition = PQGUID(0xb48d859a, 0xf5f5, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_value_type = PQGUID(0xb48d859c, 0xf5f5, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_quantity_units = PQGUID(0xb48d859b, 0xf5f5, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_quantity_characteristic = PQGUID(0x3d786f9e, 0xf76e, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_quantity_resolution = PQGUID(0xfb228ee0, 0xfc8d, 0x11d2,
	0xb4, 0x9a, 0x00, 0x60, 0x08, 0xb3, 0x71, 0x83);
constexpr auto tag_storage_method = PQGUID(0xb48d85a1, 0xf5f5, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_series_nominal = PQGUID(0x0fa118c8, 0xcb4a, 0x11d2,
	0xb3, 0x0b, 0xfe, 0x25, 0xcb, 0x9a, 0x17, 0x60);

constexpr auto tag_effective = PQGUID(0x62f28183, 0xf9c4, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_time_installed = PQGUID(0x3d786f85, 0xf76e, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_use_calibration = PQGUID(0x62f28180, 0xf9c4, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_use_transducer = PQGUID(0x62f28181, 0xf9c4, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_channel_settings = PQGUID(0x62f28182, 0xf9c4, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_nominal_frequency = PQGUID(0x0fa118c3, 0xcb4a, 0x11d2,
	0xb3, 0x0b, 0xfe, 0x25, 0xcb, 0x9a, 0x17, 0x60);
constexpr auto tag_setting_connection = PQGUID(0x9f256ee0, 0x803b, 0x11d3,
	0xb9, 0x2f, 0x00, 0x50, 0xda, 0x2b, 0x1f, 0x4d);
constexpr auto tag_one_channel_setting = PQGUID(0x3d786f9a, 0xf76e, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_channel_definition_index = PQGUID(0xb48d858f, 0xf5f5, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_xd_system_ratio = PQGUID(0x62f2818a, 0xf9c4, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_xd_monitor_ratio = PQGUID(0x62f2818b, 0xf9c4, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_cal_time_skew = PQGUID(0x62f2818d, 0xf9c4, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_cal_offset = PQGUID(0x62f2818e, 0xf9c4, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_cal_ratio = PQGUID(0x62f2818f, 0xf9c4, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);

constexpr auto tag_observation_name = PQGUID(0x3d786f8a, 0xf76e, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_time_create = PQGUID(0x3d786f8b, 0xf76e, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_time_start = PQGUID(0x3d786f8c, 0xf76e, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_trigger_method = PQGUID(0x3d786f8d, 0xf76e, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_time_triggered = PQGUID(0x3d786f8e, 0xf76e, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_channel_trigger_index = PQGUID(0x3d786f8f, 0xf76e, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_observation_serial = PQGUID(0x3d786f90, 0xf76e, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_channel_instances = PQGUID(0x3d786f91, 0xf76e, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_one_channel_instance = PQGUID(0x3d786f92, 0xf76e, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_characteristic_duration = PQGUID(0x2747d444, 0x2bd0, 0x11d2,
	0xae, 0x42, 0x00, 0x60, 0x08, 0x3a, 0x26, 0x28);
constexpr auto tag_series_instances = PQGUID(0x3d786f93, 0xf76e, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_one_series_instance = PQGUID(0x3d786f94, 0xf76e, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_series_base = PQGUID(0x3d786f95, 0xf76e, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_series_scale = PQGUID(0x3d786f96, 0xf76e, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_series_offset = PQGUID(0x3d786f97, 0xf76e, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_series_share_channel = PQGUID(0x8973861f, 0xf1c3, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_series_share_series = PQGUID(0x89738620, 0xf1c3, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto tag_series_values = PQGUID(0x3d786f99, 0xf76e, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);

constexpr auto id_data_source_measure = PQGUID(0xe6b51730, 0xf747, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto id_quantity_waveform = PQGUID(0x67f6af80, 0xf753, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto id_value = PQGUID(0x67f6af97, 0xf753, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto id_time = PQGUID(0xc690e862, 0xf755, 0x11cf,
	0x9d, 0x89, 0x00, 0x80, 0xc7, 0x2e, 0x70, 0xa3);
constexpr auto id_status = PQGUID(0xb82b5c82, 0x55c7, 0x11d5,
	0xa4, 0xb3, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
constexpr auto id_characteristic_instantaneous = PQGUID(0xa6b31add, 0xb451,
	0x11d1,
	0xae, 0x17, 0x00, 0x60, 0x08, 0x3a, 0x26, 0x28);
constexpr auto id_characteristic_status = PQGUID(0xb82b5c83, 0x55c7, 0x11d5,
	0xa4, 0xb3, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);

/* Private namespace and tags are stable product-neutral schema identifiers. */
constexpr auto private_namespace = PQGUID(0x8df54ba2, 0xd3cf, 0x5b73,
	0xa9, 0x2e, 0x8a, 0x7d, 0x71, 0x0d, 0x97, 0x3a);

constexpr std::uint8_t element_collection = 1;
constexpr std::uint8_t element_scalar = 2;
constexpr std::uint8_t element_vector = 3;
constexpr std::uint8_t physical_boolean4 = 3;
constexpr std::uint8_t physical_char1 = 10;
constexpr std::uint8_t physical_integer4 = 22;
constexpr std::uint8_t physical_unsigned4 = 32;
constexpr std::uint8_t physical_real8 = 41;
constexpr std::uint8_t physical_timestamp = 50;
constexpr std::uint8_t physical_guid = 60;

using Consumer = std::function<void(std::span<const std::byte>)>;
using ValueEmitter = std::function<void(const Consumer &, bool)>;

void append_u32(std::vector<std::byte> &output, std::uint32_t value)
{
	for (unsigned shift = 0; shift != 32; shift += 8)
		output.push_back(static_cast<std::byte>(value >> shift));
}

void append_u64(std::vector<std::byte> &output, std::uint64_t value)
{
	for (unsigned shift = 0; shift != 64; shift += 8)
		output.push_back(static_cast<std::byte>(value >> shift));
}

std::vector<std::byte> le_u32(std::uint32_t value)
{
	std::vector<std::byte> output;
	append_u32(output, value);
	return output;
}

std::vector<std::byte> le_real8(double value)
{
	std::vector<std::byte> output;
	append_u64(output, std::bit_cast<std::uint64_t>(value));
	return output;
}

std::array<std::byte, 16> guid_wire(const Uuid &value)
{
	return {value[3], value[2], value[1], value[0], value[5], value[4],
		value[7], value[6], value[8], value[9], value[10], value[11],
		value[12], value[13], value[14], value[15]};
}

std::vector<std::byte> guid_data(const Uuid &value)
{
	const auto wire = guid_wire(value);
	return {wire.begin(), wire.end()};
}

std::string clean_string(std::string_view input, std::size_t maximum = 16 * 1024)
{
	std::string output;
	output.reserve(std::min(input.size(), maximum));
	for (const unsigned char character : input) {
		if (output.size() == maximum)
			break;
		if (character == 0u || (character < 0x20u && character != '\t' &&
		    character != '\r' && character != '\n'))
			output.push_back(' ');
		else if (character < 0x80u)
			output.push_back(static_cast<char>(character));
		else
			output.push_back('?');
	}
	return output;
}

std::string hex(std::span<const std::byte> bytes)
{
	constexpr char digits[] = "0123456789abcdef";
	std::string output;
	output.reserve(bytes.size() * 2u);
	for (const auto byte : bytes) {
		const auto value = std::to_integer<std::uint8_t>(byte);
		output.push_back(digits[value >> 4u]);
		output.push_back(digits[value & 0x0fu]);
	}
	return output;
}

double rational(std::int64_t numerator, std::uint64_t denominator)
{
	using boost::multiprecision::cpp_dec_float_50;
	if (denominator == 0)
		throw ConversionError(ConversionErrorCode::source_invalid,
			"PQDIF metadata contains a zero rational denominator");
	cpp_dec_float_50 value(numerator);
	value /= cpp_dec_float_50(denominator);
	return value.convert_to<double>();
}

double rational(std::uint64_t numerator, std::uint64_t denominator)
{
	using boost::multiprecision::cpp_dec_float_50;
	if (denominator == 0)
		throw ConversionError(ConversionErrorCode::source_invalid,
			"PQDIF metadata contains a zero rational denominator");
	cpp_dec_float_50 value(numerator);
	value /= cpp_dec_float_50(denominator);
	return value.convert_to<double>();
}

struct ExactTime {
	boost::multiprecision::cpp_int numerator;
	boost::multiprecision::cpp_int denominator;
};

const TimebaseSegment &segment_for_frame(const WaveformMetadata &metadata,
	std::uint64_t frame)
{
	const auto found = std::ranges::find_if(metadata.timebase_segments,
		[frame](const auto &segment) {
			return frame >= segment.first_frame &&
				frame - segment.first_frame < segment.frame_count;
		});
	if (found == metadata.timebase_segments.end())
		throw ConversionError(ConversionErrorCode::source_invalid,
			"PQDIF sample frame has no timebase segment");
	return *found;
}

std::uint64_t sequence_for_frame(const WaveformMetadata &metadata,
	std::uint64_t frame)
{
	const auto &segment = segment_for_frame(metadata, frame);
	const auto local = frame - segment.first_frame;
	if (local > (std::numeric_limits<std::uint64_t>::max() -
		segment.first_sequence) / segment.sequence_step)
		throw ConversionError(ConversionErrorCode::source_invalid,
			"PQDIF source sequence overflows");
	return segment.first_sequence + local * segment.sequence_step;
}

std::pair<std::uint64_t, std::uint64_t> sequence_range_for_frame(
	const WaveformMetadata &metadata, std::uint64_t frame)
{
	using boost::multiprecision::cpp_int;
	const auto &segment = segment_for_frame(metadata, frame);
	const auto local = frame - segment.first_frame;
	const cpp_int first = cpp_int(segment.first_sequence) +
		cpp_int(local) * segment.sequence_step;
	const cpp_int segment_last = cpp_int(segment.first_sequence) +
		segment.source_frame_count - 1u;
	cpp_int last = segment_last;
	if (local + 1u != segment.frame_count) {
		const cpp_int complete_group_last = first + segment.sequence_step - 1u;
		if (complete_group_last < last)
			last = complete_group_last;
	}
	if (first < 0 || last < first ||
	    last > std::numeric_limits<std::uint64_t>::max())
		throw ConversionError(ConversionErrorCode::source_invalid,
			"PQDIF sample source-sequence range is invalid");
	return {first.convert_to<std::uint64_t>(),
		last.convert_to<std::uint64_t>()};
}

ExactTime utc_for_sequence(const TimebaseSegment &segment,
	std::uint64_t sequence)
{
	using boost::multiprecision::cpp_int;
	if (segment.correlation_utc_nanoseconds == 0 ||
	    segment.acquisition_rate.numerator == 0 ||
	    segment.acquisition_rate.denominator == 0)
		throw ConversionError(ConversionErrorCode::source_not_ready,
			"PQDIF source lacks exact UTC timing", {"timebase.utc_correlation"});
	const cpp_int denominator = segment.acquisition_rate.numerator;
	cpp_int numerator = cpp_int(segment.correlation_utc_nanoseconds) * denominator;
	const cpp_int delta = sequence >= segment.correlation_sequence
		? cpp_int(sequence - segment.correlation_sequence)
		: -cpp_int(segment.correlation_sequence - sequence);
	numerator += delta * segment.acquisition_rate.denominator * 1'000'000'000ull;
	return {std::move(numerator), denominator};
}

ExactTime utc_for_frame(const WaveformMetadata &metadata, std::uint64_t frame)
{
	const auto &segment = segment_for_frame(metadata, frame);
	return utc_for_sequence(segment, sequence_for_frame(metadata, frame));
}

ExactTime subtract(const ExactTime &left, const ExactTime &right)
{
	return {left.numerator * right.denominator -
		right.numerator * left.denominator,
		left.denominator * right.denominator};
}

ExactTime add(const ExactTime &left, const ExactTime &right)
{
	return {left.numerator * right.denominator +
		right.numerator * left.denominator,
		left.denominator * right.denominator};
}

double seconds_value(const ExactTime &nanoseconds)
{
	using boost::multiprecision::cpp_dec_float_50;
	if (nanoseconds.denominator <= 0)
		throw ConversionError(ConversionErrorCode::timestamp_out_of_range,
			"PQDIF timestamp rational has an invalid denominator");
	cpp_dec_float_50 seconds(nanoseconds.numerator);
	seconds /= cpp_dec_float_50(nanoseconds.denominator);
	seconds /= cpp_dec_float_50(1'000'000'000u);
	return seconds.convert_to<double>();
}

std::vector<std::byte> timestamp_data(const ExactTime &nanoseconds)
{
	using boost::multiprecision::cpp_int;
	constexpr std::uint64_t day_nanoseconds = 86'400'000'000'000ull;
	constexpr std::int64_t unix_epoch_day = 25'569;
	if (nanoseconds.denominator <= 0)
		throw ConversionError(ConversionErrorCode::timestamp_out_of_range,
			"PQDIF timestamp rational has an invalid denominator");
	const cpp_int denominator = nanoseconds.denominator * day_nanoseconds;
	cpp_int unix_day = nanoseconds.numerator / denominator;
	if (nanoseconds.numerator < 0 &&
	    nanoseconds.numerator % denominator != 0)
		--unix_day;
	const cpp_int day = unix_day + unix_epoch_day;
	if (day < 0 || day > std::numeric_limits<std::uint32_t>::max())
		throw ConversionError(ConversionErrorCode::timestamp_out_of_range,
			"PQDIF UTC timestamp lies outside its day field");
	const ExactTime within_day{
		nanoseconds.numerator - unix_day * denominator,
		nanoseconds.denominator};
	const double seconds = seconds_value(within_day);
	std::vector<std::byte> output;
	append_u32(output, day.convert_to<std::uint32_t>());
	const auto real = le_real8(seconds);
	output.insert(output.end(), real.begin(), real.end());
	return output;
}

struct Element {
	Uuid tag{};
	std::uint8_t kind = element_collection;
	std::uint8_t physical = 0;
	std::vector<std::byte> scalar_data;
	std::uint32_t vector_count = 0;
	std::uint64_t vector_data_bytes = 0;
	ValueEmitter emit_values;
	std::vector<Element> children;
	std::uint32_t offset = 0;
	std::uint32_t size = 0;
	bool embedded = false;
};

Element scalar(Uuid tag, std::uint8_t physical, std::vector<std::byte> value)
{
	Element result;
	result.tag = tag;
	result.kind = element_scalar;
	result.physical = physical;
	result.scalar_data = std::move(value);
	return result;
}

Element scalar_u32(Uuid tag, std::uint32_t value)
{
	return scalar(tag, physical_unsigned4, le_u32(value));
}

Element scalar_bool(Uuid tag, bool value)
{
	return scalar(tag, physical_boolean4, le_u32(value ? 1u : 0u));
}

Element scalar_real(Uuid tag, double value)
{
	return scalar(tag, physical_real8, le_real8(value));
}

Element scalar_guid(Uuid tag, Uuid value)
{
	return scalar(tag, physical_guid, guid_data(value));
}

Element scalar_timestamp(Uuid tag, const ExactTime &value)
{
	return scalar(tag, physical_timestamp, timestamp_data(value));
}

Element vector_element(Uuid tag, std::uint8_t physical, std::uint32_t count,
	std::uint64_t bytes, ValueEmitter emitter)
{
	Element result;
	result.tag = tag;
	result.kind = element_vector;
	result.physical = physical;
	result.vector_count = count;
	result.vector_data_bytes = bytes;
	result.emit_values = std::move(emitter);
	return result;
}

Element vector_bytes(Uuid tag, std::uint8_t physical, std::uint32_t count,
	std::vector<std::byte> values)
{
	const auto size = values.size();
	return vector_element(tag, physical, count, size,
		[values = std::move(values)](const Consumer &consumer, bool) {
			consumer(values);
		});
}

Element vector_u32(Uuid tag, std::span<const std::uint32_t> values)
{
	if (values.size() > std::numeric_limits<std::uint32_t>::max())
		throw ConversionError(ConversionErrorCode::output_too_large,
			"PQDIF UINT4 vector count exceeds UINT4");
	std::vector<std::byte> bytes;
	bytes.reserve(values.size() * sizeof(std::uint32_t));
	for (const auto value : values)
		append_u32(bytes, value);
	return vector_bytes(tag, physical_unsigned4,
		static_cast<std::uint32_t>(values.size()),
		std::move(bytes));
}

Element string_element(Uuid tag, std::string_view input)
{
	auto value = clean_string(input);
	value.push_back('\0');
	std::vector<std::byte> bytes(value.size());
	std::memcpy(bytes.data(), value.data(), value.size());
	return vector_bytes(tag, physical_char1,
		static_cast<std::uint32_t>(bytes.size()), std::move(bytes));
}

Element collection(Uuid tag, std::vector<Element> children)
{
	/* PQDIF reference readers perform binary search over collection entries.
	 * The normative C implementation orders them by the GUID's wire bytes. */
	std::stable_sort(children.begin(), children.end(),
		[](const Element &left, const Element &right) {
			const auto left_wire = guid_wire(left.tag);
			const auto right_wire = guid_wire(right.tag);
			return std::lexicographical_compare(left_wire.begin(), left_wire.end(),
				right_wire.begin(), right_wire.end());
		});
	Element result;
	result.tag = tag;
	result.kind = element_collection;
	result.children = std::move(children);
	return result;
}

std::uint32_t checked_u32(std::uint64_t value, std::string_view field)
{
	if (value > std::numeric_limits<std::uint32_t>::max())
		throw ConversionError(ConversionErrorCode::output_too_large,
			std::string(field) + " exceeds the PQDIF 32-bit size limit");
	return static_cast<std::uint32_t>(value);
}

std::uint64_t aligned_four(std::uint64_t value)
{
	if (value > std::numeric_limits<std::uint64_t>::max() - 3u)
		throw ConversionError(ConversionErrorCode::output_too_large,
			"PQDIF element size overflows");
	return (value + 3u) & ~std::uint64_t{3u};
}

struct Layout {
	std::vector<Element *> blocks;
	std::uint32_t bytes = 0;
};

void assign_collection(Element &value, std::uint64_t offset,
	std::uint64_t &cursor, Layout &layout)
{
	if (value.kind != element_collection)
		throw ConversionError(ConversionErrorCode::internal_error,
			"PQDIF layout root is not a collection");
	value.offset = checked_u32(offset, "PQDIF collection offset");
	value.size = checked_u32(4u + value.children.size() * 28ull,
		"PQDIF collection size");
	layout.blocks.push_back(&value);
	cursor = offset + value.size;
	for (auto &child : value.children) {
		if (child.kind == element_scalar && child.scalar_data.size() <= 8u) {
			child.embedded = true;
			continue;
		}
		child.offset = checked_u32(cursor, "PQDIF element offset");
		if (child.kind == element_collection) {
			assign_collection(child, cursor, cursor, layout);
			continue;
		}
		const auto raw_size = child.kind == element_scalar
			? child.scalar_data.size()
			: 4ull + child.vector_data_bytes;
		child.size = checked_u32(aligned_four(raw_size), "PQDIF element size");
		layout.blocks.push_back(&child);
		cursor += child.size;
	}
}

Layout layout(Element &root)
{
	Layout result;
	std::uint64_t cursor = 0;
	assign_collection(root, 0, cursor, result);
	result.bytes = checked_u32(cursor, "PQDIF record body");
	return result;
}

void consume_vector(const Consumer &consumer, const std::vector<std::byte> &bytes)
{
	if (!bytes.empty())
		consumer(bytes);
}

void emit_guid(const Consumer &consumer, const Uuid &value)
{
	const auto wire = guid_wire(value);
	consumer(wire);
}

void emit_padding(const Consumer &consumer, std::size_t bytes)
{
	static constexpr std::array<std::byte, 4> zero{};
	if (bytes != 0)
		consumer(std::span{zero}.first(bytes));
}

void emit_collection_block(const Element &value, const Consumer &consumer)
{
	consume_vector(consumer, le_u32(static_cast<std::uint32_t>(
		value.children.size())));
	for (const auto &child : value.children) {
		emit_guid(consumer, child.tag);
		const std::array<std::byte, 4> descriptor{
			static_cast<std::byte>(child.kind),
			static_cast<std::byte>(child.physical),
			static_cast<std::byte>(child.embedded ? 1u : 0u), std::byte{0}};
		consumer(descriptor);
		if (child.embedded) {
			consumer(child.scalar_data);
			emit_padding(consumer, 8u - child.scalar_data.size());
		} else {
			std::vector<std::byte> link;
			append_u32(link, child.offset);
			append_u32(link, child.size);
			consumer(link);
		}
	}
}

void emit_block(const Element &value, const Consumer &consumer, bool report)
{
	std::uint64_t emitted = 0;
	const Consumer counted = [&](std::span<const std::byte> bytes) {
		emitted += bytes.size();
		consumer(bytes);
	};
	if (value.kind == element_collection) {
		emit_collection_block(value, counted);
	} else if (value.kind == element_scalar) {
		counted(value.scalar_data);
	} else {
		consume_vector(counted, le_u32(value.vector_count));
		if (value.emit_values)
			value.emit_values(counted, report);
	}
	if (emitted > value.size)
		throw ConversionError(ConversionErrorCode::internal_error,
			"PQDIF element emitted more bytes than its layout");
	emit_padding(counted, static_cast<std::size_t>(value.size - emitted));
	if (emitted != value.size)
		throw ConversionError(ConversionErrorCode::internal_error,
			"PQDIF element emitted an invalid byte count");
}

void emit_body(const Layout &layout, const Consumer &consumer, bool report)
{
	std::uint64_t cursor = 0;
	for (const auto *block : layout.blocks) {
		if (block->offset != cursor)
			throw ConversionError(ConversionErrorCode::internal_error,
				"PQDIF layout contains a gap or overlap");
		emit_block(*block, consumer, report);
		cursor += block->size;
	}
	if (cursor != layout.bytes)
		throw ConversionError(ConversionErrorCode::internal_error,
			"PQDIF layout byte count is inconsistent");
}

struct EncodedStats {
	std::uint32_t bytes = 0;
	std::uint32_t checksum = 0;
};

EncodedStats encode_body(const Layout &layout, bool compressed,
	OutputSink *destination, bool report)
{
	std::uint64_t output_bytes = 0;
	uLong checksum = ::adler32(0L, Z_NULL, 0);
	const Consumer output = [&](std::span<const std::byte> bytes) {
		for (std::size_t offset = 0; offset < bytes.size();) {
			const auto count = static_cast<uInt>(std::min<std::size_t>(
				bytes.size() - offset, std::numeric_limits<uInt>::max()));
			checksum = ::adler32(checksum,
				reinterpret_cast<const Bytef *>(bytes.data() + offset), count);
			if (destination)
				destination->write(bytes.subspan(offset, count));
			output_bytes += count;
			offset += count;
		}
	};

	if (!compressed) {
		emit_body(layout, output, report);
	} else {
		z_stream stream{};
		if (::deflateInit(&stream, Z_BEST_COMPRESSION) != Z_OK)
			throw ConversionError(ConversionErrorCode::internal_error,
				"cannot initialize PQDIF zlib compression");
		std::array<std::byte, 64 * 1024> compressed_bytes{};
		try {
			const Consumer input = [&](std::span<const std::byte> bytes) {
				for (std::size_t offset = 0; offset < bytes.size();) {
					const auto count = static_cast<uInt>(std::min<std::size_t>(
						bytes.size() - offset, std::numeric_limits<uInt>::max()));
					stream.next_in = reinterpret_cast<Bytef *>(
						const_cast<std::byte *>(bytes.data() + offset));
					stream.avail_in = count;
					while (stream.avail_in != 0u) {
						stream.next_out = reinterpret_cast<Bytef *>(
							compressed_bytes.data());
						stream.avail_out = compressed_bytes.size();
						if (::deflate(&stream, Z_NO_FLUSH) != Z_OK)
							throw ConversionError(ConversionErrorCode::internal_error,
								"PQDIF zlib compression failed");
						output(std::span{compressed_bytes}.first(
							compressed_bytes.size() - stream.avail_out));
					}
					offset += count;
				}
			};
			emit_body(layout, input, report);
			int status = Z_OK;
			while (status != Z_STREAM_END) {
				stream.next_out = reinterpret_cast<Bytef *>(compressed_bytes.data());
				stream.avail_out = compressed_bytes.size();
				status = ::deflate(&stream, Z_FINISH);
				if (status != Z_OK && status != Z_STREAM_END)
					throw ConversionError(ConversionErrorCode::internal_error,
						"PQDIF zlib finalization failed");
				output(std::span{compressed_bytes}.first(
					compressed_bytes.size() - stream.avail_out));
			}
			::deflateEnd(&stream);
		} catch (...) {
			::deflateEnd(&stream);
			throw;
		}
	}
	return {checked_u32(output_bytes, "PQDIF encoded record body"),
		static_cast<std::uint32_t>(checksum)};
}

void write_header(OutputSink &sink, Uuid record_type, std::uint32_t body_bytes,
	std::uint32_t next_record, std::uint32_t checksum)
{
	std::vector<std::byte> header;
	const auto signature = guid_wire(record_signature);
	const auto type = guid_wire(record_type);
	header.insert(header.end(), signature.begin(), signature.end());
	header.insert(header.end(), type.begin(), type.end());
	append_u32(header, 64u);
	append_u32(header, body_bytes);
	append_u32(header, next_record);
	append_u32(header, checksum);
	for (unsigned index = 0; index != 4; ++index)
		append_u32(header, 0u);
	if (header.size() != 64u)
		throw ConversionError(ConversionErrorCode::internal_error,
			"PQDIF record header geometry is invalid");
	sink.write(header);
}

std::uint32_t phase_id(Phase phase)
{
	return static_cast<std::uint32_t>(phase);
}

std::uint32_t measured_id(Quantity quantity)
{
	switch (quantity) {
	case Quantity::voltage: return 1;
	case Quantity::current: return 2;
	case Quantity::status: return 17;
	/* Frequency is represented by its Hertz unit. PQDIF has no frequency
	 * tagQuantityMeasuredID; value 5 means temperature. */
	case Quantity::frequency: return 0;
	case Quantity::ratio:
	case Quantity::unknown: return 0;
	}
	return 0;
}

std::uint32_t units_id(SiUnit unit)
{
	switch (unit) {
	case SiUnit::volt: return 6;
	case SiUnit::ampere: return 7;
	case SiUnit::hertz: return 15;
	case SiUnit::dimensionless: return 0;
	}
	return 0;
}

std::uint64_t event_end(const EventDescriptor &event)
{
	if ((event.flags & event_end_valid) != 0u)
		return event.end_sequence;
	if ((event.flags & event_current_valid) != 0u)
		return event.current_sequence;
	return event.start_sequence;
}

std::uint64_t event_active_end(const EventDescriptor &event,
	std::uint64_t selection_last_sequence)
{
	return (event.flags & event_end_valid) != 0u
		? event.end_sequence : selection_last_sequence;
}

const TimebaseSegment &segment_for_sequence(const WaveformMetadata &metadata,
	std::uint64_t sequence)
{
	using boost::multiprecision::cpp_int;
	const auto found = std::ranges::find_if(metadata.timebase_segments,
		[sequence](const auto &segment) {
			return sequence >= segment.first_sequence &&
				cpp_int(sequence - segment.first_sequence) <
					segment.source_frame_count;
		});
	if (found == metadata.timebase_segments.end())
		throw ConversionError(ConversionErrorCode::source_invalid,
			"PQDIF event sequence lies outside the selected timebase");
	return *found;
}

double event_duration_seconds(const WaveformMetadata &metadata,
	const EventDescriptor &event)
{
	const auto last_sequence = event_end(event);
	if (last_sequence < event.start_sequence)
		throw ConversionError(ConversionErrorCode::source_invalid,
			"PQDIF event end precedes its start");
	const auto &first_segment = segment_for_sequence(metadata,
		event.start_sequence);
	const auto &last_segment = segment_for_sequence(metadata, last_sequence);
	const auto first = utc_for_sequence(first_segment, event.start_sequence);
	const auto last = utc_for_sequence(last_segment, last_sequence);
	const ExactTime final_sample_period{
		boost::multiprecision::cpp_int(last_segment.acquisition_rate.denominator) *
			1'000'000'000ull,
		last_segment.acquisition_rate.numerator};
	const auto duration = subtract(add(last, final_sample_period), first);
	if (duration.numerator < 0)
		throw ConversionError(
			ConversionErrorCode::source_discontinuity_unsupported,
			"PQDIF event duration crosses a backwards UTC discontinuity");
	return seconds_value(duration);
}

std::string utc_offset_name(std::int32_t offset_seconds)
{
	const auto absolute = std::llabs(static_cast<long long>(offset_seconds));
	std::ostringstream output;
	output << "UTC" << (offset_seconds < 0 ? '-' : '+')
	       << std::setw(2) << std::setfill('0') << absolute / 3600 << ':'
	       << std::setw(2) << (absolute % 3600) / 60;
	if (absolute % 60 != 0)
		output << ':' << std::setw(2) << absolute % 60;
	return output.str();
}

std::uint32_t physical_connection(Topology topology)
{
	switch (topology) {
	case Topology::wye: return 4u;   // ID_3ELMENT_WYE (normative spelling)
	case Topology::delta: return 5u; // ID_3ELEMENT_DELTA
	case Topology::unknown:
		throw ConversionError(ConversionErrorCode::source_not_ready,
			"PQDIF source has no captured physical connection", {"topology"});
	}
	throw ConversionError(ConversionErrorCode::source_invalid,
		"PQDIF source has an unknown physical connection");
}

std::uint32_t phase_mask(Phase phase)
{
	switch (phase) {
	case Phase::a: return 1u << 0u;
	case Phase::b: return 1u << 1u;
	case Phase::c: return 1u << 2u;
	case Phase::neutral: return 1u << 3u;
	case Phase::ab: return (1u << 0u) | (1u << 1u);
	case Phase::bc: return (1u << 1u) | (1u << 2u);
	case Phase::ca: return (1u << 2u) | (1u << 0u);
	case Phase::none: return 0u;
	}
	return 0u;
}

std::uint32_t trigger_method(const EventDescriptor *trigger)
{
	if (trigger == nullptr)
		return 0u; // ID_TRIGGER_METH_NONE
	if (trigger->trigger_source == 3u)
		return 1u; // ID_TRIGGER_METH_CHANNEL
	return 3u; // ID_TRIGGER_METH_EXTERNAL (manual or unknown source)
}

std::vector<std::uint32_t> trigger_channels(
	const WaveformMetadata &metadata, const EventDescriptor &trigger)
{
	std::vector<std::uint32_t> result;
	const bool system = (trigger.phase_mask & (1u << 4u)) != 0u;
	for (std::size_t index = 0; index != metadata.channels.size(); ++index) {
		const auto &channel = metadata.channels[index];
		const bool quantity_matches =
			trigger.quantity == Quantity::unknown ||
			trigger.quantity == channel.quantity;
		const bool phase_matches = system || trigger.phase_mask == 0u ||
			(trigger.phase_mask & phase_mask(channel.phase)) != 0u;
		if (quantity_matches && phase_matches)
			result.push_back(checked_u32(index,
				"PQDIF trigger channel index"));
	}
	if (!result.empty())
		return result;

	const auto event = std::ranges::find_if(metadata.events,
		[&trigger](const auto &candidate) {
			return candidate.event_uuid == trigger.event_uuid;
		});
	if (event != metadata.events.end()) {
		const auto event_index = static_cast<std::size_t>(
			std::distance(metadata.events.begin(), event));
		result.push_back(checked_u32(metadata.channels.size() + event_index,
			"PQDIF trigger status channel index"));
	}
	return result;
}

const EventDescriptor *trigger_event(const WaveformMetadata &metadata,
	const ConversionOptions &options)
{
	if (metadata.events.empty())
		return nullptr;
	if (options.scope == ExportScope::event) {
		const auto selected = options.selected_event_uuid
			? options.selected_event_uuid : metadata.selected_event_uuid;
		const auto found = std::ranges::find_if(metadata.events,
			[&](const auto &event) {
				return selected && event.event_uuid == *selected;
			});
		if (found == metadata.events.end())
			throw ConversionError(ConversionErrorCode::source_event_not_found,
				"selected PQDIF trigger event is absent");
		return &*found;
	}
	return &*std::ranges::min_element(metadata.events, {},
		[](const auto &event) { return event.start_sequence; });
}

ExactTime trigger_time(const WaveformMetadata &metadata,
	const EventDescriptor &event)
{
	if ((event.flags & event_trigger_valid) != 0u) {
		for (const auto &segment : metadata.timebase_segments) {
			const auto end = boost::multiprecision::cpp_int(segment.first_sequence) +
				segment.source_frame_count;
			if (event.trigger_sequence >= segment.first_sequence &&
			    boost::multiprecision::cpp_int(event.trigger_sequence) < end)
				return utc_for_sequence(segment, event.trigger_sequence);
		}
	}
	if ((event.flags & event_utc_valid) != 0u && event.trigger_utc_nanoseconds != 0)
		return {event.trigger_utc_nanoseconds, 1};
	throw ConversionError(ConversionErrorCode::source_not_ready,
		"PQDIF trigger lacks captured UTC timing", {"event.trigger_utc"});
}

std::string metadata_comments(const WaveformMetadata &metadata,
	const ConversionOptions &options, const EventDescriptor *trigger)
{
	std::ostringstream output;
	output << "profile=IEEE 1159.3-2025; definitions=pqdif-normative/1.0.0\n"
	       << "source.capture_uuid="
	       << uuid_string(uuid_is_zero(metadata.capture.source_capture_uuid)
			? metadata.capture.capture_uuid : metadata.capture.source_capture_uuid)
	       << "\nselection.capture_uuid=" << uuid_string(metadata.capture.capture_uuid)
	       << "\nselection.scope="
	       << (options.scope == ExportScope::capture ? "capture" : "event")
	       << "\nsource.device_uuid=" << uuid_string(metadata.capture.device_uuid)
	       << "\nsource.configuration_sha256="
	       << hex(metadata.capture.configuration_sha256)
	       << "\nsource.sensor_profile_sha256="
	       << hex(metadata.capture.sensor_profile_sha256)
	       << "\nsource.configuration_id="
	       << clean_string(metadata.capture.configuration_id)
	       << "\nsource.sensor_profile_id="
	       << clean_string(metadata.capture.sensor_profile_id)
	       << "\nsource.software_build_id="
	       << clean_string(metadata.capture.software_build_id)
	       << "\ntopology=" << static_cast<unsigned>(metadata.capture.topology)
	       << "\ncalibration.status="
	       << static_cast<unsigned>(metadata.capture.calibration_status)
	       << "\ncalibration.id=" << clean_string(metadata.capture.calibration_id);
	if (trigger)
		output << "\ntrigger.event_uuid=" << uuid_string(trigger->event_uuid);
	for (std::size_t index = 0; index != metadata.timebase_segments.size(); ++index) {
		const auto &segment = metadata.timebase_segments[index];
		output << "\ntimebase." << index << '=' << segment.first_frame << '+'
		       << segment.frame_count << ",sequences="
		       << segment.first_sequence << '+' << segment.source_frame_count
		       << ",step=" << segment.sequence_step
		       << ",rate=" << segment.persisted_rate.numerator << '/'
		       << segment.persisted_rate.denominator << ",acquisition_rate="
		       << segment.acquisition_rate.numerator << '/'
		       << segment.acquisition_rate.denominator << ",decimation="
		       << segment.decimation_divisor << ':'
		       << static_cast<unsigned>(segment.decimation_method)
		       << ",correlation_sequence=" << segment.correlation_sequence
		       << ",correlation_utc_ns=" << segment.correlation_utc_nanoseconds
		       << ",utc_offset_s=" << segment.utc_offset_seconds
		       << ",clock=" << static_cast<unsigned>(segment.clock_source)
		       << ",quality=" << static_cast<unsigned>(segment.time_quality)
		       << ",flags=" << segment.flags
		       << ",uncertainty_ns=" << segment.uncertainty_nanoseconds;
	}
	for (std::size_t index = 0; index != metadata.channels.size(); ++index) {
		const auto &channel = metadata.channels[index];
		output << "\nchannel." << index << "=uuid:"
		       << uuid_string(channel.stable_id) << ",source:"
		       << channel.source_channel << ",phase:"
		       << static_cast<unsigned>(channel.phase) << ",quantity:"
		       << static_cast<unsigned>(channel.quantity) << ",unit:"
		       << static_cast<unsigned>(channel.si_unit) << ",flags:"
		       << channel.flags << ",bits:" << channel.valid_bits << '/'
		       << channel.storage_bits << ",gain:" << channel.gain.numerator
		       << '/' << channel.gain.denominator << ",offset:"
		       << channel.offset.numerator << '/' << channel.offset.denominator
		       << ",ratio:" << channel.primary_secondary_ratio.numerator << '/'
		       << channel.primary_secondary_ratio.denominator << ",nominal:"
		       << channel.nominal.numerator << '/' << channel.nominal.denominator
		       << ",range:" << channel.range_minimum.numerator << '/'
		       << channel.range_minimum.denominator << ':'
		       << channel.range_maximum.numerator << '/'
		       << channel.range_maximum.denominator << ",clip:"
		       << channel.clipping_low << ':' << channel.clipping_high;
	}
	for (const auto &event : metadata.events)
		output << "\nevent=" << uuid_string(event.event_uuid) << ",start="
		       << event.start_sequence << ",end=" << event_end(event)
		       << ",flags=" << event.flags << ",status=" << event.status
		       << ",lifecycle=" << static_cast<unsigned>(event.lifecycle)
		       << ",phase_mask=" << event.phase_mask << ",quantity="
		       << static_cast<unsigned>(event.quantity) << ",trigger_source="
		       << event.trigger_source << ",severity=" << event.severity
		       << ",uncertainty_ns=" << event.uncertainty_nanoseconds
		       << ",reference_u=" << event.reference_micro_units
		       << ",threshold_u=" << event.threshold_micro_units
		       << ",hysteresis_u=" << event.hysteresis_micro_units
		       << ",extrema_u=" << event.extrema_micro_units[0] << ':'
		       << event.extrema_micro_units[1] << ':'
		       << event.extrema_micro_units[2] << ",duration_samples="
		       << event.duration_samples << ",updates=" << event.update_count
		       << ",configuration_generation="
		       << event.configuration_generation
		       << ",taxonomy=" << clean_string(event.taxonomy_name, 256)
		       << ",label=" << clean_string(event.label, 256);
	for (const auto &event : metadata.events)
		if (!event.settings_snapshot_json.empty())
			output << "\nevent.settings." << uuid_string(event.event_uuid) << '='
			       << clean_string(event.settings_snapshot_json, 4096);
	for (const auto &quality : metadata.quality_intervals)
		output << "\nquality=frames:" << quality.first_frame << '+'
		       << quality.frame_count << ",sequences:" << quality.first_sequence
		       << '-' << quality.last_sequence << ",flags:" << quality.flags
		       << ",channels:" << quality.channel_mask << ",severity:"
		       << quality.severity << ",source:" << quality.source
		       << ",detail:" << quality.detail_code;
	for (const auto &entry : metadata.lineage)
		output << "\nlineage=relation:" << static_cast<unsigned>(entry.relation)
		       << ",capture:" << uuid_string(entry.related_capture_uuid)
		       << ",event:" << uuid_string(entry.related_event_uuid)
		       << ",sequences:" << entry.first_sequence << '-'
		       << entry.last_sequence << ",part:" << entry.part_index << '/'
		       << entry.part_count;
	if (!metadata.capture.comments.empty())
		output << "\nsource.comments=" << clean_string(metadata.capture.comments, 4096);
	return clean_string(output.str(), 60 * 1024);
}

struct PqdifChannel {
	std::string name;
	std::string identifier;
	Phase phase = Phase::none;
	Quantity quantity = Quantity::status;
	SiUnit unit = SiUnit::dimensionless;
	std::uint32_t source_index = 0;
	std::optional<std::size_t> event_index;
	bool quality = false;
	SignedRational gain{1, 1};
	SignedRational offset{0, 1};
	UnsignedRational ratio{1, 1};
	std::optional<SignedRational> nominal;
	std::optional<UnsignedRational> resolution;
};

std::vector<PqdifChannel> pqdif_channels(const WaveformMetadata &metadata)
{
	std::vector<PqdifChannel> result;
	result.reserve(metadata.channels.size() + metadata.events.size() + 1u);
	for (std::size_t index = 0; index != metadata.channels.size(); ++index) {
		const auto &source = metadata.channels[index];
		PqdifChannel channel;
		channel.name = source.name;
		channel.identifier = uuid_string(source.stable_id);
		channel.phase = source.phase;
		channel.quantity = source.quantity;
		channel.unit = source.si_unit;
		channel.source_index = source.source_channel;
		channel.gain = source.gain;
		channel.offset = source.offset;
		channel.ratio = source.primary_secondary_ratio;
		if ((source.flags & channel_nominal_valid) != 0u)
			channel.nominal = source.nominal;
		if ((source.flags & channel_resolution_valid) != 0u)
			channel.resolution = source.resolution;
		result.push_back(std::move(channel));
	}
	for (std::size_t index = 0; index != metadata.events.size(); ++index) {
		PqdifChannel channel;
		channel.name = "Event " + clean_string(metadata.events[index].label, 96);
		channel.identifier = uuid_string(metadata.events[index].event_uuid);
		channel.event_index = index;
		result.push_back(std::move(channel));
	}
	PqdifChannel quality;
	quality.name = "MNCWF quality flags";
	quality.identifier = "mncwf-quality-flags";
	quality.quality = true;
	result.push_back(std::move(quality));
	return result;
}

Element series_definition(bool time, const PqdifChannel &channel)
{
	const bool status = !time && (channel.event_index.has_value() || channel.quality);
	std::vector<Element> children;
	children.push_back(scalar_guid(tag_value_type,
		time ? id_time : (status ? id_status : id_value)));
	children.push_back(scalar_u32(tag_quantity_units,
		time ? 2u : units_id(channel.unit)));
	children.push_back(scalar_guid(tag_quantity_characteristic,
		status ? id_characteristic_status : id_characteristic_instantaneous));
	children.push_back(scalar_u32(tag_storage_method,
		time || status ? 1u : 2u));
	if (!time && channel.nominal)
		children.push_back(scalar_real(tag_series_nominal,
			rational(channel.nominal->numerator, channel.nominal->denominator)));
	if (!time && channel.resolution)
		children.push_back(scalar_real(tag_quantity_resolution,
			rational(channel.resolution->numerator,
				channel.resolution->denominator)));
	return collection(tag_one_series_definition, std::move(children));
}

Element make_data_source(const WaveformMetadata &metadata,
	const std::vector<PqdifChannel> &channels, Uuid object_id)
{
	std::vector<Element> definitions;
	definitions.reserve(channels.size());
	for (std::size_t index = 0; index != channels.size(); ++index) {
		const auto &channel = channels[index];
		std::vector<Element> child;
		child.push_back(string_element(tag_channel_name, channel.name));
		child.push_back(scalar_u32(tag_phase, phase_id(channel.phase)));
		child.push_back(scalar_guid(tag_quantity_type, id_quantity_waveform));
		child.push_back(scalar_u32(tag_quantity_measured,
			measured_id(channel.quantity)));
		child.push_back(string_element(tag_other_channel_id, channel.identifier));
		if (!metadata.capture.circuit_name.empty())
			child.push_back(string_element(tag_group_name,
				metadata.capture.circuit_name));
		child.push_back(scalar_u32(tag_physical_channel,
			index < metadata.channels.size() ? channel.source_index :
				checked_u32(index, "PQDIF derived physical channel")));
		child.push_back(scalar_u32(tag_primary_series, 1u));
		std::vector<Element> series;
		series.push_back(series_definition(true, channel));
		series.push_back(series_definition(false, channel));
		child.push_back(collection(tag_series_definitions, std::move(series)));
		definitions.push_back(collection(tag_one_channel_definition,
			std::move(child)));
	}
	std::vector<Element> root;
	root.push_back(scalar_guid(tag_data_source_type, id_data_source_measure));
	if (!metadata.capture.device_serial.empty())
		root.push_back(string_element(tag_serial, metadata.capture.device_serial));
	root.push_back(string_element(tag_source_version,
		metadata.capture.firmware_version));
	root.push_back(string_element(tag_source_name,
		metadata.capture.product_name.empty() ? metadata.capture.device_model
			: metadata.capture.product_name));
	const auto &location = metadata.capture.site_name.empty()
		? metadata.capture.station_name : metadata.capture.site_name;
	if (!location.empty())
		root.push_back(string_element(tag_location, location));
	root.push_back(string_element(tag_time_zone, utc_offset_name(
		metadata.timebase_segments.front().utc_offset_seconds)));
	root.push_back(scalar_real(tag_utc_to_lst,
		static_cast<double>(
			metadata.timebase_segments.front().utc_offset_seconds)));
	root.push_back(collection(tag_channel_definitions, std::move(definitions)));
	root.push_back(scalar_guid(uuid_v5(private_namespace, "tag.object-id"),
		object_id));
	return collection(rec_data_source, std::move(root));
}

Element make_settings(const WaveformMetadata &metadata,
	const std::vector<PqdifChannel> &channels, const ExactTime &start,
	Uuid object_id, std::string_view comments)
{
	std::vector<Element> settings;
	settings.reserve(channels.size());
	for (std::size_t index = 0; index != channels.size(); ++index) {
		std::vector<Element> child;
		child.push_back(scalar_u32(tag_channel_definition_index,
			static_cast<std::uint32_t>(index)));
		if (index < metadata.channels.size()) {
			const auto &channel = channels[index];
			child.push_back(scalar_real(tag_cal_time_skew, 0.0));
			child.push_back(scalar_real(tag_cal_offset,
				rational(channel.offset.numerator, channel.offset.denominator)));
			child.push_back(scalar_real(tag_cal_ratio,
				rational(channel.gain.numerator, channel.gain.denominator)));
			child.push_back(scalar_real(tag_xd_system_ratio,
				static_cast<double>(channel.ratio.numerator)));
			child.push_back(scalar_real(tag_xd_monitor_ratio,
				static_cast<double>(channel.ratio.denominator)));
		}
		settings.push_back(collection(tag_one_channel_setting, std::move(child)));
	}
	const bool ratios = std::ranges::any_of(metadata.channels,
		[](const auto &channel) {
			return channel.primary_secondary_ratio.numerator != 1u ||
				channel.primary_secondary_ratio.denominator != 1u;
		});
	std::vector<Element> root;
	root.push_back(scalar_timestamp(tag_effective, start));
	root.push_back(scalar_timestamp(tag_time_installed, start));
	root.push_back(scalar_bool(tag_use_calibration,
		metadata.capture.calibration_status == CalibrationStatus::valid &&
		std::ranges::all_of(metadata.channels, [](const auto &channel) {
			return (channel.flags & channel_calibration_valid) != 0u;
		})));
	root.push_back(scalar_bool(tag_use_transducer, ratios));
	root.push_back(collection(tag_channel_settings, std::move(settings)));
	root.push_back(scalar_real(tag_nominal_frequency,
		rational(metadata.capture.nominal_frequency.numerator,
			metadata.capture.nominal_frequency.denominator)));
	root.push_back(scalar_u32(tag_setting_connection,
		physical_connection(metadata.capture.topology)));
	root.push_back(string_element(tag_comments, comments));
	root.push_back(scalar_guid(uuid_v5(private_namespace, "tag.object-id"),
		object_id));
	return collection(rec_settings, std::move(root));
}

Element make_container(const WaveformMetadata &metadata,
	const ConversionOptions &options, const ExactTime &start, Uuid object_id,
	std::string_view comments)
{
	std::vector<std::byte> version;
	append_u32(version, 1u);
	append_u32(version, 7u);
	append_u32(version, 1u);
	append_u32(version, 0u);
	std::string stem = clean_string(options.output_stem, 96);
	if (stem.empty()) stem = "waveform";
	std::vector<Element> root;
	root.push_back(vector_bytes(tag_version, physical_unsigned4, 4u,
		std::move(version)));
	root.push_back(string_element(tag_file_name, stem + ".pqd"));
	root.push_back(scalar_timestamp(tag_creation,
		metadata.capture.created_utc_nanoseconds != 0
			? ExactTime{metadata.capture.created_utc_nanoseconds, 1} : start));
	root.push_back(string_element(tag_comments, comments));
	root.push_back(scalar_u32(tag_compression_style,
		options.pqdif_record_compression ? 2u : 0u));
	root.push_back(scalar_u32(tag_compression_algorithm,
		options.pqdif_record_compression ? 1u : 0u));
	root.push_back(scalar_guid(uuid_v5(private_namespace, "tag.object-id"),
		object_id));
	return collection(rec_container, std::move(root));
}

Element time_values(const WaveformMetadata &metadata, std::uint64_t frames,
	const ExactTime &start, std::stop_token stop_token)
{
	return vector_element(tag_series_values, physical_real8,
		checked_u32(frames, "PQDIF time-series point count"), frames * 8u,
		[&metadata, frames, start, stop_token](const Consumer &consumer, bool) {
			constexpr std::size_t batch = 1024;
			std::vector<std::byte> output;
			output.reserve(batch * 8u);
			for (std::uint64_t frame = 0; frame < frames;) {
				if (stop_token.stop_requested())
					throw ConversionError(ConversionErrorCode::conversion_cancelled,
						"waveform conversion was cancelled");
				const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(
					batch, frames - frame));
				output.clear();
				for (std::size_t local = 0; local != count; ++local) {
					const auto timestamp = utc_for_frame(metadata, frame + local);
					const auto encoded = le_real8(seconds_value(
						subtract(timestamp, start)));
					output.insert(output.end(), encoded.begin(), encoded.end());
				}
				consumer(output);
				frame += count;
			}
		});
}

Element source_values(const WaveformSource &source, std::size_t channel,
	const ConversionOptions &options, std::stop_token stop_token,
	ProgressCallback progress, int &progress_pass)
{
	const auto frames = source.frame_count();
	return vector_element(tag_series_values, physical_integer4,
		checked_u32(frames, "PQDIF waveform point count"), frames * 4u,
		[&source, channel, frames, batch_size = options.frame_batch_size,
		 stop_token, progress = std::move(progress), &progress_pass]
		(const Consumer &consumer, bool report) {
			const auto channels = source.channel_count();
			const auto capacity = static_cast<std::size_t>(std::min<std::uint64_t>(
				batch_size, frames));
			std::vector<std::int64_t> input(capacity * channels);
			std::vector<std::byte> output;
			output.reserve(capacity * 4u);
			std::uint64_t frame = 0;
			while (frame < frames) {
				if (stop_token.stop_requested())
					throw ConversionError(ConversionErrorCode::conversion_cancelled,
						"waveform conversion was cancelled");
				const auto requested = static_cast<std::size_t>(
					std::min<std::uint64_t>(capacity, frames - frame));
				const auto produced = source.read_frames(frame, requested,
					std::span<std::int64_t>{input}.first(requested * channels));
				if (produced == 0 || produced > requested)
					throw ConversionError(ConversionErrorCode::source_invalid,
						"waveform source ended before its declared frame count");
				output.clear();
				for (std::size_t local = 0; local != produced; ++local) {
					const auto sample = input[local * channels + channel];
					if (sample < std::numeric_limits<std::int32_t>::min() ||
					    sample > std::numeric_limits<std::int32_t>::max())
						throw ConversionError(ConversionErrorCode::sample_out_of_range,
							"source sample cannot be represented by PQDIF INT4");
					append_u32(output, std::bit_cast<std::uint32_t>(
						static_cast<std::int32_t>(sample)));
				}
				consumer(output);
				frame += produced;
				if (report && channel == 0u && progress) {
					const auto base = progress_pass == 0 ? 0u : frames / 2u;
					const auto portion = frame / 2u;
					progress({std::min(frames, base + portion), frames});
				}
			}
		});
}

Element event_values(const WaveformMetadata &metadata, std::size_t event_index,
	std::uint64_t frames, std::stop_token stop_token)
{
	return vector_element(tag_series_values, physical_unsigned4,
		checked_u32(frames, "PQDIF event-status point count"), frames * 4u,
		[&metadata, event_index, frames, stop_token]
		(const Consumer &consumer, bool) {
			const auto &event = metadata.events[event_index];
			constexpr std::size_t batch = 4096;
			std::vector<std::byte> output;
			output.reserve(batch * 4u);
			for (std::uint64_t frame = 0; frame < frames;) {
				if (stop_token.stop_requested())
					throw ConversionError(ConversionErrorCode::conversion_cancelled,
						"waveform conversion was cancelled");
				const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(
					batch, frames - frame));
				output.clear();
				for (std::size_t local = 0; local != count; ++local) {
					const auto [first_sequence, last_sequence] =
						sequence_range_for_frame(metadata, frame + local);
					append_u32(output,
						first_sequence <= event_active_end(
							 event, metadata.last_sequence) &&
						last_sequence >= event.start_sequence ? 1u : 0u);
				}
				consumer(output);
				frame += count;
			}
		});
}

Element quality_values(const WaveformMetadata &metadata, std::uint64_t frames,
	std::stop_token stop_token)
{
	return vector_element(tag_series_values, physical_unsigned4,
		checked_u32(frames, "PQDIF quality point count"), frames * 4u,
		[&metadata, frames, stop_token](const Consumer &consumer, bool) {
			constexpr std::size_t batch = 4096;
			std::vector<std::byte> output;
			output.reserve(batch * 4u);
			for (std::uint64_t frame = 0; frame < frames;) {
				if (stop_token.stop_requested())
					throw ConversionError(ConversionErrorCode::conversion_cancelled,
						"waveform conversion was cancelled");
				const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(
					batch, frames - frame));
				output.clear();
				for (std::size_t local = 0; local != count; ++local) {
					std::uint32_t flags = 0;
					for (const auto &interval : metadata.quality_intervals)
						if (frame + local >= interval.first_frame &&
						    frame + local - interval.first_frame < interval.frame_count)
							flags |= interval.flags;
					append_u32(output, flags);
				}
				consumer(output);
				frame += count;
			}
		});
}

Element make_observation(const WaveformSource &source,
	const std::vector<PqdifChannel> &channels, const ConversionOptions &options,
	const ExactTime &start, const EventDescriptor *trigger, Uuid object_id,
	std::string_view comments, std::stop_token stop_token,
	ProgressCallback progress, int &progress_pass)
{
	const auto &metadata = source.metadata();
	std::vector<Element> instances;
	instances.reserve(channels.size());
	for (std::size_t index = 0; index != channels.size(); ++index) {
		const auto &channel = channels[index];
		std::vector<Element> series;
		std::vector<Element> time_instance;
		if (index == 0u)
			time_instance.push_back(time_values(metadata, source.frame_count(),
				start, stop_token));
		else {
			time_instance.push_back(scalar_u32(tag_series_share_channel, 0u));
			time_instance.push_back(scalar_u32(tag_series_share_series, 0u));
		}
		series.push_back(collection(tag_one_series_instance,
			std::move(time_instance)));

		std::vector<Element> value_instance;
		const bool status = channel.event_index.has_value() || channel.quality;
		if (!status) {
			value_instance.push_back(scalar_real(tag_series_scale,
				rational(channel.gain.numerator, channel.gain.denominator)));
			value_instance.push_back(scalar_real(tag_series_offset,
				rational(channel.offset.numerator, channel.offset.denominator)));
		}
		if (!status && channel.nominal)
			value_instance.push_back(scalar_real(tag_series_base,
				rational(channel.nominal->numerator, channel.nominal->denominator)));
		if (index < metadata.channels.size())
			value_instance.push_back(source_values(source, index, options,
				stop_token, progress, progress_pass));
		else if (channel.event_index)
			value_instance.push_back(event_values(metadata, *channel.event_index,
				source.frame_count(), stop_token));
		else
			value_instance.push_back(quality_values(metadata, source.frame_count(),
				stop_token));
		series.push_back(collection(tag_one_series_instance,
			std::move(value_instance)));

		std::vector<Element> instance;
		instance.push_back(scalar_u32(tag_channel_definition_index,
			static_cast<std::uint32_t>(index)));
		if (channel.event_index)
			instance.push_back(scalar_real(tag_characteristic_duration,
				event_duration_seconds(metadata,
					metadata.events[*channel.event_index])));
		instance.push_back(collection(tag_series_instances, std::move(series)));
		instances.push_back(collection(tag_one_channel_instance,
			std::move(instance)));
	}

	std::vector<Element> root;
	root.push_back(string_element(tag_observation_name,
		options.scope == ExportScope::event && trigger
			? trigger->label : "MNCWF capture"));
	root.push_back(scalar_timestamp(tag_time_create,
		metadata.capture.created_utc_nanoseconds != 0
			? ExactTime{metadata.capture.created_utc_nanoseconds, 1} : start));
	root.push_back(scalar_timestamp(tag_time_start, start));
	const auto method = trigger_method(trigger);
	root.push_back(scalar_u32(tag_trigger_method, method));
	if (trigger) {
		root.push_back(scalar_timestamp(tag_time_triggered,
			trigger_time(metadata, *trigger)));
		if (method == 1u) {
			const auto indices = trigger_channels(metadata, *trigger);
			if (!indices.empty())
				root.push_back(vector_u32(tag_channel_trigger_index,
					std::span<const std::uint32_t>{indices}));
		}
	}
	root.push_back(scalar_u32(tag_observation_serial,
		static_cast<std::uint32_t>(metadata.first_sequence & 0xffffffffu)));
	root.push_back(collection(tag_channel_instances, std::move(instances)));
	root.push_back(string_element(tag_comments, comments));
	root.push_back(scalar_guid(uuid_v5(private_namespace, "tag.object-id"),
		object_id));
	return collection(rec_observation, std::move(root));
}

struct Record {
	Uuid type{};
	Element root;
	Layout body;
	bool compressed = false;
	EncodedStats stats{};
};

} // namespace

ConversionSummary PqdifConverter::convert(const WaveformSource &source,
	OutputSink &sink, const ConversionOptions &options, std::stop_token stop_token,
	ProgressCallback progress) const
{
	if (options.format != ExportFormat::pqdif)
		throw ConversionError(ConversionErrorCode::invalid_options,
			"PQDIF converter received a different output format");
	validate_conversion_source(source, options);
	if (source.frame_count() > std::numeric_limits<std::uint32_t>::max())
		throw ConversionError(ConversionErrorCode::output_too_large,
			"PQDIF series point count exceeds UINT4");
	if (stop_token.stop_requested())
		throw ConversionError(ConversionErrorCode::conversion_cancelled,
			"waveform conversion was cancelled");
	const auto &metadata = source.metadata();
	std::vector<std::string> missing;
	if (metadata.capture.topology == Topology::unknown)
		missing.push_back("capture.topology");
	for (std::size_t index = 0; index != metadata.channels.size(); ++index) {
		const auto &channel = metadata.channels[index];
		if ((channel.flags & channel_transform_valid) == 0u)
			missing.push_back("channel[" + std::to_string(index) + "].affine_transform");
		if ((channel.flags & channel_ratio_valid) == 0u)
			missing.push_back("channel[" + std::to_string(index) + "].primary_secondary_ratio");
		if ((channel.flags & channel_nominal_valid) == 0u)
			missing.push_back("channel[" + std::to_string(index) + "].nominal");
		if ((channel.flags & channel_resolution_valid) == 0u)
			missing.push_back("channel[" + std::to_string(index) + "].resolution");
		if (channel.gain.denominator == 0 || channel.offset.denominator == 0 ||
		    channel.primary_secondary_ratio.denominator == 0 ||
		    channel.nominal.denominator == 0 ||
		    channel.resolution.denominator == 0)
			missing.push_back("channel[" + std::to_string(index) +
				"].rational_denominator");
	}
	for (std::size_t index = 0; index != metadata.timebase_segments.size(); ++index) {
		if ((metadata.timebase_segments[index].flags & time_utc_offset_known) == 0u)
			missing.push_back("timebase[" + std::to_string(index) + "].utc_context");
		if (metadata.timebase_segments[index].clock_source == ClockSource::unknown)
			missing.push_back("timebase[" + std::to_string(index) + "].clock_source");
		if (metadata.timebase_segments[index].time_quality == TimeQuality::unknown)
			missing.push_back("timebase[" + std::to_string(index) + "].time_quality");
	}
	if (!missing.empty())
		throw ConversionError(ConversionErrorCode::source_not_ready,
			"MNCWF source is not ready for PQDIF conversion", std::move(missing));
	if (std::ranges::any_of(metadata.timebase_segments,
		[expected = metadata.timebase_segments.front().utc_offset_seconds]
		(const auto &segment) {
			return segment.utc_offset_seconds != expected;
		}))
		throw ConversionError(
			ConversionErrorCode::source_discontinuity_unsupported,
			"PQDIF interval crosses a captured UTC-offset discontinuity");

	if (progress)
		progress({0, source.frame_count()});
	const auto start = utc_for_frame(metadata, 0);
	for (std::size_t index = 1; index != metadata.timebase_segments.size(); ++index) {
		const auto frame = metadata.timebase_segments[index].first_frame;
		const auto boundary_delta = subtract(utc_for_frame(metadata, frame),
			utc_for_frame(metadata, frame - 1u));
		if (boundary_delta.numerator < 0)
			throw ConversionError(
				ConversionErrorCode::source_discontinuity_unsupported,
				"PQDIF UTC timebase moves backwards at a segment boundary");
	}
	const auto *trigger = trigger_event(metadata, options);
	const auto comments = metadata_comments(metadata, options, trigger);
	const auto source_uuid = uuid_is_zero(metadata.capture.source_capture_uuid)
		? metadata.capture.capture_uuid : metadata.capture.source_capture_uuid;
	const auto selection = uuid_string(metadata.capture.capture_uuid) + ':' +
		std::string(options.scope == ExportScope::capture ? "capture" : "event") + ':' +
		(trigger ? uuid_string(trigger->event_uuid) : "none");
	const auto container_id = uuid_v5(source_uuid, "pqdif/container/" + selection);
	const auto data_source_id = uuid_v5(source_uuid, "pqdif/data-source");
	const auto settings_id = uuid_v5(source_uuid, "pqdif/settings/" + selection);
	const auto observation_id = uuid_v5(source_uuid, "pqdif/observation/" + selection);
	const auto channels = pqdif_channels(metadata);
	int progress_pass = 0;

	std::vector<Record> records;
	records.reserve(4);
	records.push_back({rec_container,
		make_container(metadata, options, start, container_id, comments), {}, false, {}});
	records.push_back({rec_data_source,
		make_data_source(metadata, channels, data_source_id), {},
		options.pqdif_record_compression, {}});
	records.push_back({rec_settings,
		make_settings(metadata, channels, start, settings_id, comments), {},
		options.pqdif_record_compression, {}});
	records.push_back({rec_observation,
		make_observation(source, channels, options, start, trigger, observation_id,
			comments, stop_token, progress, progress_pass), {},
		options.pqdif_record_compression, {}});

	std::uint64_t total_bytes = 0;
	for (auto &record : records) {
		record.body = layout(record.root);
		record.stats = encode_body(record.body, record.compressed, nullptr,
			record.type == rec_observation);
		total_bytes += 64ull + record.stats.bytes;
		if (total_bytes > options.maximum_output_bytes ||
		    total_bytes > sink.byte_limit())
			throw ConversionError(ConversionErrorCode::output_too_large,
				"PQDIF export exceeds its configured output limit");
	}

	progress_pass = 1;
	std::uint64_t offset = 0;
	for (std::size_t index = 0; index != records.size(); ++index) {
		auto &record = records[index];
		const auto next = index + 1u == records.size() ? 0u : checked_u32(
			offset + 64ull + record.stats.bytes, "PQDIF next-record link");
		write_header(sink, record.type, record.stats.bytes, next,
			record.stats.checksum);
		const auto written = encode_body(record.body, record.compressed, &sink,
			record.type == rec_observation);
		if (written.bytes != record.stats.bytes ||
		    written.checksum != record.stats.checksum)
			throw ConversionError(ConversionErrorCode::validation_failed,
				"PQDIF deterministic compression validation failed");
		offset += 64ull + record.stats.bytes;
	}
	if (sink.bytes_written() != total_bytes)
		throw ConversionError(ConversionErrorCode::validation_failed,
			"PQDIF output byte count is invalid");
	if (progress)
		progress({source.frame_count(), source.frame_count()});
	return {ExportFormat::pqdif,
		"IEEE 1159.3-2025 PQDIF (normative definitions 1.0.0)",
		source.frame_count(), sink.bytes_written(), ".pqd",
		"application/vnd.pqdif"};
}

} // namespace mnc::waveform
