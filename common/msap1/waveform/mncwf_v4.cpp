#include "msap1/waveform/mncwf_v4.hpp"
#include "msap1/meter/meter_data.hpp"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <cstring>
#include <limits>
#include <numeric>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <openssl/sha.h>
#include <sys/random.h>

namespace msap1 {
namespace {

[[noreturn]] void reject(std::string_view reason)
{
	throw std::invalid_argument("MNCWF v4: " + std::string(reason));
}

std::uint32_t crc32c_update(std::uint32_t crc,
	std::span<const std::byte> bytes) noexcept
{
	constexpr std::uint32_t polynomial = 0x82F63B78u;
	for (const auto value : bytes) {
		crc ^= std::to_integer<std::uint8_t>(value);
		for (unsigned bit = 0; bit < 8u; ++bit)
			crc = (crc >> 1u) ^
				((crc & 1u) != 0u ? polynomial : 0u);
	}
	return crc;
}

std::uint64_t checked_add(std::uint64_t left, std::uint64_t right,
	std::string_view label)
{
	if (right > std::numeric_limits<std::uint64_t>::max() - left)
		reject(std::string(label) + " overflows");
	return left + right;
}

std::uint64_t checked_multiply(std::uint64_t left, std::uint64_t right,
	std::string_view label)
{
	if (left != 0u &&
	    right > std::numeric_limits<std::uint64_t>::max() / left)
		reject(std::string(label) + " overflows");
	return left * right;
}

std::uint64_t align_eight(std::uint64_t value)
{
	return checked_add(value, 7u, "alignment") & ~std::uint64_t{7u};
}

std::size_t narrow_size(std::uint64_t value, std::string_view label)
{
	if (value > std::numeric_limits<std::size_t>::max())
		reject(std::string(label) + " exceeds the host address space");
	return static_cast<std::size_t>(value);
}

std::span<const std::byte> range(std::span<const std::byte> bytes,
	std::uint64_t offset, std::uint64_t length, std::string_view label)
{
	const auto end = checked_add(offset, length, label);
	if (end > bytes.size())
		reject(std::string(label) + " is truncated");
	return bytes.subspan(narrow_size(offset, label), narrow_size(length, label));
}

std::uint16_t read_u16(std::span<const std::byte> bytes, std::size_t offset)
{
	if (offset > bytes.size() || bytes.size() - offset < 2u)
		reject("truncated 16-bit field");
	return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset])) |
	       (static_cast<std::uint16_t>(
			std::to_integer<std::uint8_t>(bytes[offset + 1u]))
		<< 8u);
}

std::int16_t read_s16(std::span<const std::byte> bytes, std::size_t offset)
{
	return std::bit_cast<std::int16_t>(read_u16(bytes, offset));
}

std::uint32_t read_u32(std::span<const std::byte> bytes, std::size_t offset)
{
	if (offset > bytes.size() || bytes.size() - offset < 4u)
		reject("truncated 32-bit field");
	std::uint32_t value = 0;
	for (unsigned byte = 0; byte < 4u; ++byte)
		value |= static_cast<std::uint32_t>(
				 std::to_integer<std::uint8_t>(bytes[offset + byte]))
			 << (byte * 8u);
	return value;
}

std::int32_t read_s32(std::span<const std::byte> bytes, std::size_t offset)
{
	return std::bit_cast<std::int32_t>(read_u32(bytes, offset));
}

std::uint64_t read_u64(std::span<const std::byte> bytes, std::size_t offset)
{
	if (offset > bytes.size() || bytes.size() - offset < 8u)
		reject("truncated 64-bit field");
	std::uint64_t value = 0;
	for (unsigned byte = 0; byte < 8u; ++byte)
		value |= static_cast<std::uint64_t>(
				 std::to_integer<std::uint8_t>(bytes[offset + byte]))
			 << (byte * 8u);
	return value;
}

std::int64_t read_s64(std::span<const std::byte> bytes, std::size_t offset)
{
	return std::bit_cast<std::int64_t>(read_u64(bytes, offset));
}

template<std::size_t Size>
std::array<std::byte, Size> read_array(std::span<const std::byte> bytes,
	std::size_t offset)
{
	if (offset > bytes.size() || bytes.size() - offset < Size)
		reject("truncated byte-array field");
	std::array<std::byte, Size> result{};
	std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), Size,
		result.begin());
	return result;
}

template<std::size_t Size>
bool all_zero(const std::array<std::byte, Size> &value)
{
	return std::ranges::all_of(value,
		[](std::byte byte) { return byte == std::byte{0}; });
}

bool all_zero(std::span<const std::byte> value)
{
	return std::ranges::all_of(value,
		[](std::byte byte) { return byte == std::byte{0}; });
}

bool valid_utf8(std::span<const std::byte> value)
{
	std::size_t index = 0;
	while (index < value.size()) {
		const auto first = std::to_integer<std::uint8_t>(value[index]);
		if (first == 0u)
			return false;
		if (first < 0x80u) {
			++index;
			continue;
		}

		std::size_t continuation = 0;
		std::uint32_t codepoint = 0;
		std::uint32_t minimum = 0;
		if ((first & 0xe0u) == 0xc0u) {
			continuation = 1;
			codepoint = first & 0x1fu;
			minimum = 0x80u;
		} else if ((first & 0xf0u) == 0xe0u) {
			continuation = 2;
			codepoint = first & 0x0fu;
			minimum = 0x800u;
		} else if ((first & 0xf8u) == 0xf0u) {
			continuation = 3;
			codepoint = first & 0x07u;
			minimum = 0x10000u;
		} else {
			return false;
		}
		if (continuation > value.size() - index - 1u)
			return false;
		for (std::size_t tail = 1; tail <= continuation; ++tail) {
			const auto next =
				std::to_integer<std::uint8_t>(value[index + tail]);
			if ((next & 0xc0u) != 0x80u)
				return false;
			codepoint = (codepoint << 6u) | (next & 0x3fu);
		}
		if (codepoint < minimum || codepoint > 0x10ffffu ||
		    (codepoint >= 0xd800u && codepoint <= 0xdfffu))
			return false;
		index += continuation + 1u;
	}
	return true;
}

std::string read_string(std::span<const std::byte> record, std::size_t ref_offset,
	std::span<const std::byte> blob, std::string_view label)
{
	const auto offset = read_u32(record, ref_offset);
	const auto bytes = read_u32(record, ref_offset + 4u);
	if (bytes > mncwf_v4_max_string_bytes)
		reject(std::string(label) + " exceeds the string bound");
	if (bytes == 0u && offset != 0u)
		reject(std::string(label) + " has a noncanonical empty reference");
	const auto text = range(blob, offset, bytes, label);
	if (text.empty())
		return {};
	if (!valid_utf8(text))
		reject(std::string(label) + " is not canonical UTF-8");
	return {reinterpret_cast<const char *>(text.data()), text.size()};
}

bool known_section(std::uint32_t type)
{
	return type >= static_cast<std::uint32_t>(
			MncwfV4SectionType::capture_metadata) &&
	       type <= static_cast<std::uint32_t>(MncwfV4SectionType::sample_data);
}

std::size_t section_index(std::uint32_t type)
{
	return static_cast<std::size_t>(type - 1u);
}

struct ParsedSection {
	std::span<const std::byte> records;
	std::span<const std::byte> blob;
};

ParsedSection parse_section_envelope(const MncwfV4SectionInfo &section,
	std::uint32_t expected_item_bytes, std::uint64_t minimum_count,
	std::uint64_t maximum_count, bool allow_blob)
{
	if (section.version != 1u)
		reject("unsupported mandatory-section version");
	if (section.flags != mncwf_v4_section_required)
		reject("mandatory section is not marked required");
	if (section.item_bytes != expected_item_bytes ||
	    section.item_count < minimum_count ||
	    section.item_count > maximum_count)
		reject("mandatory-section geometry is invalid");
	if (section.data.size() < mncwf_v4_section_header_bytes)
		reject("mandatory section header is truncated");
	if (read_u32(section.data, 0) != section.type ||
	    read_u16(section.data, 4) != section.version ||
	    read_u16(section.data, 6) != mncwf_v4_section_header_bytes ||
	    read_u32(section.data, 8) != 0u ||
	    read_u32(section.data, 12) != section.item_bytes ||
	    read_u64(section.data, 16) != section.item_count ||
	    read_u64(section.data, 40) != 0u)
		reject("mandatory section envelope disagrees with its directory entry");

	const auto records_bytes = checked_multiply(section.item_count,
		section.item_bytes, "section records");
	const auto records_end = checked_add(mncwf_v4_section_header_bytes,
		records_bytes, "section records");
	const auto blob_offset = read_u64(section.data, 24);
	const auto blob_bytes = read_u64(section.data, 32);
	if (blob_bytes == 0u) {
		if (blob_offset != 0u || records_end != section.stored_bytes)
			reject("section without a blob has trailing data");
	} else {
		if (!allow_blob || blob_offset != align_eight(records_end) ||
		    checked_add(blob_offset, blob_bytes, "section blob") !=
			    section.stored_bytes)
			reject("section blob geometry is invalid");
		const auto padding = range(section.data, records_end,
			blob_offset - records_end, "section blob padding");
		if (!all_zero(padding))
			reject("section blob padding is nonzero");
	}

	return {
		range(section.data, mncwf_v4_section_header_bytes, records_bytes,
			"section records"),
		blob_bytes == 0u
			? std::span<const std::byte>{}
			: range(section.data, blob_offset, blob_bytes, "section blob"),
	};
}

bool valid_topology(std::uint32_t value)
{
	return value <= static_cast<std::uint32_t>(MncwfTopology::delta);
}

bool valid_calibration(std::uint32_t value)
{
	return value <= static_cast<std::uint32_t>(
			MncwfCalibrationStatus::invalid);
}

bool valid_time_quality(std::uint16_t value)
{
	return value <= static_cast<std::uint16_t>(MncwfTimeQuality::locked);
}

bool valid_clock_source(std::uint16_t value)
{
	return value <= static_cast<std::uint16_t>(MncwfClockSource::manual);
}

bool valid_phase(std::uint16_t value)
{
	return value <= static_cast<std::uint16_t>(MncwfPhase::ca);
}

bool valid_quantity(std::uint16_t value)
{
	return value <= static_cast<std::uint16_t>(MncwfQuantity::ratio);
}

bool valid_unit(std::uint16_t value)
{
	return value <= static_cast<std::uint16_t>(MncwfSiUnit::hertz);
}

bool valid_event_taxonomy(std::uint16_t value)
{
	return value <= static_cast<std::uint16_t>(
			MncwfEventTaxonomy::product_alarm);
}

bool valid_event_lifecycle(std::uint16_t value)
{
	return value >= static_cast<std::uint16_t>(MncwfEventLifecycle::start) &&
	       value <= static_cast<std::uint16_t>(MncwfEventLifecycle::complete);
}

bool valid_lineage_relation(std::uint16_t value)
{
	return value >= static_cast<std::uint16_t>(MncwfLineageRelation::parent) &&
	       value <= static_cast<std::uint16_t>(
			MncwfLineageRelation::virtual_slice);
}

std::pair<std::uint64_t, std::uint64_t> reduced(std::uint64_t numerator,
	std::uint64_t denominator)
{
	if (numerator == 0u || denominator == 0u)
		reject("sample-rate rational is zero");
	const auto divisor = std::gcd(numerator, denominator);
	return {numerator / divisor, denominator / divisor};
}

bool rate_matches_decimation(const MncwfV4TimebaseSegment &segment)
{
	auto numerator = segment.acquisition_rate_numerator;
	auto denominator = segment.acquisition_rate_denominator;
	auto decimation = static_cast<std::uint64_t>(segment.decimation_divisor);
	const auto cancellation = std::gcd(numerator, decimation);
	numerator /= cancellation;
	decimation /= cancellation;
	denominator = checked_multiply(denominator, decimation,
		"decimated sample-rate denominator");
	return reduced(numerator, denominator) ==
	       reduced(segment.persisted_rate_numerator,
		segment.persisted_rate_denominator);
}

} // namespace

std::uint32_t mncwf_crc32c(std::span<const std::byte> bytes) noexcept
{
	return crc32c_update(0xffffffffu, bytes) ^ 0xffffffffu;
}

MncwfSha256 mncwf_sha256(std::span<const std::byte> bytes)
{
	MncwfSha256 digest{};
	if (SHA256(reinterpret_cast<const unsigned char *>(bytes.data()),
		bytes.size(), reinterpret_cast<unsigned char *>(digest.data())) ==
	    nullptr)
		throw std::runtime_error("MNCWF v4: SHA-256 failed");
	return digest;
}

MncwfUuid mncwf_random_uuid()
{
	MncwfUuid uuid{};
	std::size_t completed = 0;
	while (completed < uuid.size()) {
		const auto count = ::getrandom(uuid.data() + completed,
			uuid.size() - completed, 0);
		if (count < 0) {
			if (errno == EINTR)
				continue;
			throw std::runtime_error(std::string("MNCWF v4: kernel UUID RNG: ") +
				std::strerror(errno));
		}
		if (count == 0)
			throw std::runtime_error("MNCWF v4: kernel UUID RNG returned EOF");
		completed += static_cast<std::size_t>(count);
	}
	/* RFC 4122 variant, random/version-4 layout. */
	uuid[6] = static_cast<std::byte>(
		(std::to_integer<std::uint8_t>(uuid[6]) & 0x0fu) | 0x40u);
	uuid[8] = static_cast<std::byte>(
		(std::to_integer<std::uint8_t>(uuid[8]) & 0x3fu) | 0x80u);
	return uuid;
}

std::string mncwf_uuid_string(const MncwfUuid &uuid)
{
	constexpr char digits[] = "0123456789abcdef";
	std::string result;
	result.reserve(36u);
	for (std::size_t index = 0; index < uuid.size(); ++index) {
		if (index == 4u || index == 6u || index == 8u || index == 10u)
			result.push_back('-');
		const auto value = std::to_integer<std::uint8_t>(uuid[index]);
		result.push_back(digits[value >> 4u]);
		result.push_back(digits[value & 0x0fu]);
	}
	return result;
}

std::optional<MncwfUuid> mncwf_uuid_from_string(std::string_view text) noexcept
{
	if (text.size() != 36u || text[8] != '-' || text[13] != '-' ||
	    text[18] != '-' || text[23] != '-')
		return std::nullopt;
	const auto nibble = [](char value) -> std::optional<std::uint8_t> {
		if (value >= '0' && value <= '9')
			return static_cast<std::uint8_t>(value - '0');
		if (value >= 'a' && value <= 'f')
			return static_cast<std::uint8_t>(value - 'a' + 10);
		return std::nullopt;
	};
	MncwfUuid result{};
	std::size_t source = 0u;
	for (auto &byte : result) {
		if (source == 8u || source == 13u || source == 18u || source == 23u)
			++source;
		const auto high = nibble(text[source++]);
		const auto low = nibble(text[source++]);
		if (!high || !low)
			return std::nullopt;
		byte = static_cast<std::byte>((*high << 4u) | *low);
	}
	return result;
}

bool mncwf_uuid_is_zero(const MncwfUuid &uuid) noexcept
{
	return std::ranges::all_of(uuid,
		[](std::byte value) { return value == std::byte{0}; });
}

MncwfUuid mncwf_stable_event_uuid(std::uint64_t session,
	std::uint64_t counter)
{
	return stable_power_quality_event_uuid({session, counter});
}

namespace {

using EncodedBytes = std::vector<std::byte>;

void ensure_write(const EncodedBytes &bytes, std::size_t offset,
	std::size_t width)
{
	if (offset > bytes.size() || width > bytes.size() - offset)
		reject("encoder field exceeds its record");
}

void put_u16(EncodedBytes &bytes, std::size_t offset, std::uint16_t value)
{
	ensure_write(bytes, offset, 2u);
	for (unsigned index = 0; index < 2u; ++index)
		bytes[offset + index] = static_cast<std::byte>(
			(value >> (index * 8u)) & 0xffu);
}

void put_s16(EncodedBytes &bytes, std::size_t offset, std::int16_t value)
{
	put_u16(bytes, offset, std::bit_cast<std::uint16_t>(value));
}

void put_u32(EncodedBytes &bytes, std::size_t offset, std::uint32_t value)
{
	ensure_write(bytes, offset, 4u);
	for (unsigned index = 0; index < 4u; ++index)
		bytes[offset + index] = static_cast<std::byte>(
			(value >> (index * 8u)) & 0xffu);
}

void put_s32(EncodedBytes &bytes, std::size_t offset, std::int32_t value)
{
	put_u32(bytes, offset, std::bit_cast<std::uint32_t>(value));
}

void put_u64(EncodedBytes &bytes, std::size_t offset, std::uint64_t value)
{
	ensure_write(bytes, offset, 8u);
	for (unsigned index = 0; index < 8u; ++index)
		bytes[offset + index] = static_cast<std::byte>(
			(value >> (index * 8u)) & 0xffu);
}

void put_s64(EncodedBytes &bytes, std::size_t offset, std::int64_t value)
{
	put_u64(bytes, offset, std::bit_cast<std::uint64_t>(value));
}

template<std::size_t Size>
void put_array(EncodedBytes &bytes, std::size_t offset,
	const std::array<std::byte, Size> &value)
{
	ensure_write(bytes, offset, Size);
	std::copy(value.begin(), value.end(), bytes.begin() +
		static_cast<std::ptrdiff_t>(offset));
}

std::pair<std::uint32_t, std::uint32_t> append_string(
	EncodedBytes &blob, std::string_view value)
{
	if (value.empty())
		return {0u, 0u};
	if (value.size() > mncwf_v4_max_string_bytes ||
	    blob.size() > std::numeric_limits<std::uint32_t>::max() - value.size())
		reject("encoder string blob exceeds its bound");
	const auto offset = static_cast<std::uint32_t>(blob.size());
	const auto raw = std::as_bytes(std::span{value.data(), value.size()});
	blob.insert(blob.end(), raw.begin(), raw.end());
	return {offset, static_cast<std::uint32_t>(value.size())};
}

void put_string_ref(EncodedBytes &record, std::size_t offset,
	std::pair<std::uint32_t, std::uint32_t> reference)
{
	put_u32(record, offset, reference.first);
	put_u32(record, offset + 4u, reference.second);
}

struct EncodedSection {
	MncwfV4SectionType type{};
	std::uint64_t item_count = 0;
	std::uint32_t item_bytes = 0;
	EncodedBytes payload;
};

EncodedSection enveloped_section(MncwfV4SectionType type,
	std::uint64_t item_count, std::uint32_t item_bytes,
	EncodedBytes records, EncodedBytes blob = {})
{
	const auto expected_records = checked_multiply(item_count, item_bytes,
		"encoded section records");
	if (records.size() != expected_records)
		reject("encoder section record geometry is inconsistent");
	const auto records_end = checked_add(mncwf_v4_section_header_bytes,
		records.size(), "encoded section records");
	const auto blob_offset = blob.empty() ? 0u : align_eight(records_end);
	const auto total = blob.empty() ? records_end :
		checked_add(blob_offset, blob.size(), "encoded section blob");
	if (total > mncwf_v4_max_file_bytes)
		reject("encoded section exceeds the file bound");

	EncodedSection section{type, item_count, item_bytes,
		EncodedBytes(narrow_size(total, "encoded section"))};
	put_u32(section.payload, 0, static_cast<std::uint32_t>(type));
	put_u16(section.payload, 4, 1u);
	put_u16(section.payload, 6, mncwf_v4_section_header_bytes);
	put_u32(section.payload, 8, 0u);
	put_u32(section.payload, 12, item_bytes);
	put_u64(section.payload, 16, item_count);
	put_u64(section.payload, 24, blob_offset);
	put_u64(section.payload, 32, blob.size());
	put_u64(section.payload, 40, 0u);
	std::copy(records.begin(), records.end(), section.payload.begin() +
		static_cast<std::ptrdiff_t>(mncwf_v4_section_header_bytes));
	if (!blob.empty())
		std::copy(blob.begin(), blob.end(), section.payload.begin() +
			static_cast<std::ptrdiff_t>(blob_offset));
	return section;
}

EncodedSection encode_capture(const MncwfV4CaptureMetadata &capture)
{
	EncodedBytes record(mncwf_v4_capture_metadata_bytes);
	EncodedBytes blob;
	put_array(record, 0, capture.capture_uuid);
	put_array(record, 16, capture.device_uuid);
	put_array(record, 32, capture.configuration_sha256);
	put_array(record, 64, capture.sensor_profile_sha256);
	put_u64(record, 96, capture.created_tai_nanoseconds);
	put_u64(record, 104, capture.created_utc_nanoseconds);
	put_s64(record, 112, capture.nominal_voltage_numerator);
	put_u64(record, 120, capture.nominal_voltage_denominator);
	put_u64(record, 128, capture.nominal_frequency_numerator);
	put_u64(record, 136, capture.nominal_frequency_denominator);
	put_u32(record, 144, static_cast<std::uint32_t>(capture.topology));
	put_u32(record, 148,
		static_cast<std::uint32_t>(capture.calibration_status));
	put_u32(record, 152, capture.flags);
	const std::array<std::string_view, 12> strings{
		capture.station_name, capture.site_name, capture.circuit_name,
		capture.product_name, capture.device_model, capture.firmware_version,
		capture.software_build_id, capture.sensor_profile_id,
		capture.configuration_id, capture.calibration_id,
		capture.device_serial, capture.comments,
	};
	for (std::size_t index = 0; index < strings.size(); ++index)
		put_string_ref(record, 160u + index * 8u,
			append_string(blob, strings[index]));
	return enveloped_section(MncwfV4SectionType::capture_metadata, 1u,
		mncwf_v4_capture_metadata_bytes, std::move(record), std::move(blob));
}

EncodedSection encode_timebase(
	const std::vector<MncwfV4TimebaseSegment> &segments)
{
	EncodedBytes records(segments.size() * mncwf_v4_timebase_segment_bytes);
	for (std::size_t index = 0; index < segments.size(); ++index) {
		const auto base = index * mncwf_v4_timebase_segment_bytes;
		const auto &segment = segments[index];
		put_u64(records, base + 0, segment.first_frame);
		put_u64(records, base + 8, segment.frame_count);
		put_u64(records, base + 16, segment.first_sequence);
		put_u64(records, base + 24, segment.sequence_step);
		put_u64(records, base + 32, segment.acquisition_rate_numerator);
		put_u64(records, base + 40, segment.acquisition_rate_denominator);
		put_u64(records, base + 48, segment.persisted_rate_numerator);
		put_u64(records, base + 56, segment.persisted_rate_denominator);
		put_u64(records, base + 64, segment.correlation_sequence);
		put_u64(records, base + 72, segment.correlation_pl_tick);
		put_u64(records, base + 80, segment.correlation_tai_nanoseconds);
		put_u64(records, base + 88, segment.correlation_utc_nanoseconds);
		put_u64(records, base + 96, segment.uncertainty_nanoseconds);
		put_u32(records, base + 104, segment.decimation_divisor);
		put_u16(records, base + 108,
			static_cast<std::uint16_t>(segment.decimation_method));
		put_u16(records, base + 110,
			static_cast<std::uint16_t>(segment.clock_source));
		put_u16(records, base + 112,
			static_cast<std::uint16_t>(segment.time_quality));
		put_u16(records, base + 114, segment.flags);
		put_s32(records, base + 116, segment.utc_offset_seconds);
		put_u64(records, base + 120, segment.source_frame_count);
	}
	return enveloped_section(MncwfV4SectionType::timebase_segments,
		segments.size(), mncwf_v4_timebase_segment_bytes, std::move(records));
}

EncodedSection encode_channels(
	const std::vector<MncwfV4ChannelDefinition> &channels)
{
	EncodedBytes records(channels.size() * mncwf_v4_channel_definition_bytes);
	EncodedBytes blob;
	for (std::size_t index = 0; index < channels.size(); ++index) {
		const auto base = index * mncwf_v4_channel_definition_bytes;
		const auto &channel = channels[index];
		ensure_write(records, base, mncwf_v4_channel_definition_bytes);
		std::copy(channel.stable_id.begin(), channel.stable_id.end(),
			records.begin() + static_cast<std::ptrdiff_t>(base));
		put_u32(records, base + 16, channel.source_channel);
		put_u32(records, base + 20, channel.flags);
		put_u16(records, base + 24, static_cast<std::uint16_t>(channel.phase));
		put_u16(records, base + 26,
			static_cast<std::uint16_t>(channel.quantity));
		put_u16(records, base + 28, static_cast<std::uint16_t>(channel.si_unit));
		put_u16(records, base + 30,
			static_cast<std::uint16_t>(channel.sample_encoding));
		put_u16(records, base + 32, channel.storage_bits);
		put_u16(records, base + 34, channel.valid_bits);
		put_s16(records, base + 36, channel.display_exponent10);
		put_s64(records, base + 40, channel.gain_numerator);
		put_u64(records, base + 48, channel.gain_denominator);
		put_s64(records, base + 56, channel.offset_numerator);
		put_u64(records, base + 64, channel.offset_denominator);
		put_u64(records, base + 72,
			channel.primary_secondary_ratio_numerator);
		put_u64(records, base + 80,
			channel.primary_secondary_ratio_denominator);
		put_s64(records, base + 88, channel.nominal_numerator);
		put_u64(records, base + 96, channel.nominal_denominator);
		put_s64(records, base + 104, channel.range_minimum_numerator);
		put_u64(records, base + 112, channel.range_minimum_denominator);
		put_s64(records, base + 120, channel.range_maximum_numerator);
		put_u64(records, base + 128, channel.range_maximum_denominator);
		put_u64(records, base + 136, channel.resolution_numerator);
		put_u64(records, base + 144, channel.resolution_denominator);
		put_s64(records, base + 152, channel.clipping_low);
		put_s64(records, base + 160, channel.clipping_high);
		put_string_ref(records, base + 168,
			append_string(blob, channel.name));
		put_string_ref(records, base + 176,
			append_string(blob, channel.unit_symbol));
		put_string_ref(records, base + 184,
			append_string(blob, channel.description));
	}
	return enveloped_section(MncwfV4SectionType::channel_definitions,
		channels.size(), mncwf_v4_channel_definition_bytes,
		std::move(records), std::move(blob));
}

EncodedSection encode_events(const std::vector<MncwfV4EventDescriptor> &events)
{
	EncodedBytes records(events.size() * mncwf_v4_event_descriptor_bytes);
	EncodedBytes blob;
	for (std::size_t index = 0; index < events.size(); ++index) {
		const auto base = index * mncwf_v4_event_descriptor_bytes;
		const auto &event = events[index];
		ensure_write(records, base, mncwf_v4_event_descriptor_bytes);
		std::copy(event.event_uuid.begin(), event.event_uuid.end(),
			records.begin() + static_cast<std::ptrdiff_t>(base));
		put_u16(records, base + 16,
			static_cast<std::uint16_t>(event.taxonomy));
		put_u16(records, base + 18, event.event_type);
		put_u16(records, base + 20,
			static_cast<std::uint16_t>(event.lifecycle));
		put_u16(records, base + 22,
			static_cast<std::uint16_t>(event.time_quality));
		put_u32(records, base + 24, event.flags);
		put_u32(records, base + 28, event.phase_mask);
		put_u16(records, base + 32,
			static_cast<std::uint16_t>(event.quantity));
		put_u16(records, base + 34,
			static_cast<std::uint16_t>(event.si_unit));
		put_u16(records, base + 36, event.trigger_source);
		put_u32(records, base + 40, event.configuration_generation);
		put_u32(records, base + 44, event.severity);
		put_u64(records, base + 48, event.start_sequence);
		put_u64(records, base + 56, event.current_sequence);
		put_u64(records, base + 64, event.end_sequence);
		put_u64(records, base + 72, event.trigger_sequence);
		put_u64(records, base + 80, event.start_tai_nanoseconds);
		put_u64(records, base + 88, event.current_tai_nanoseconds);
		put_u64(records, base + 96, event.end_tai_nanoseconds);
		put_u64(records, base + 104, event.trigger_tai_nanoseconds);
		put_u64(records, base + 112, event.start_utc_nanoseconds);
		put_u64(records, base + 120, event.current_utc_nanoseconds);
		put_u64(records, base + 128, event.end_utc_nanoseconds);
		put_u64(records, base + 136, event.trigger_utc_nanoseconds);
		put_u64(records, base + 144, event.uncertainty_nanoseconds);
		put_s64(records, base + 152, event.reference_micro_units);
		put_s64(records, base + 160, event.threshold_micro_units);
		put_s64(records, base + 168, event.hysteresis_micro_units);
		for (std::size_t phase = 0; phase < 3u; ++phase)
			put_s64(records, base + 176u + phase * 8u,
				event.extrema_micro_units[phase]);
		put_u64(records, base + 200, event.duration_samples);
		put_u64(records, base + 208, event.update_count);
		put_u32(records, base + 216, event.status);
		put_string_ref(records, base + 224,
			append_string(blob, event.taxonomy_name));
		put_string_ref(records, base + 232,
			append_string(blob, event.label));
		put_string_ref(records, base + 240,
			append_string(blob, event.settings_snapshot_json));
	}
	return enveloped_section(MncwfV4SectionType::event_descriptors,
		events.size(), mncwf_v4_event_descriptor_bytes,
		std::move(records), std::move(blob));
}

EncodedSection encode_quality(
	const std::vector<MncwfV4QualityInterval> &intervals)
{
	EncodedBytes records(intervals.size() * mncwf_v4_quality_interval_bytes);
	for (std::size_t index = 0; index < intervals.size(); ++index) {
		const auto base = index * mncwf_v4_quality_interval_bytes;
		const auto &quality = intervals[index];
		put_u64(records, base + 0, quality.first_frame);
		put_u64(records, base + 8, quality.frame_count);
		put_u64(records, base + 16, quality.first_sequence);
		put_u64(records, base + 24, quality.last_sequence);
		put_u64(records, base + 32, quality.channel_mask);
		put_u32(records, base + 40, quality.flags);
		put_u16(records, base + 44, quality.severity);
		put_u16(records, base + 46, quality.source);
		put_u32(records, base + 48, quality.detail_code);
	}
	return enveloped_section(MncwfV4SectionType::quality_intervals,
		intervals.size(), mncwf_v4_quality_interval_bytes,
		std::move(records));
}

EncodedSection encode_lineage(
	const std::vector<MncwfV4LineageEntry> &entries)
{
	EncodedBytes records(entries.size() * mncwf_v4_lineage_entry_bytes);
	for (std::size_t index = 0; index < entries.size(); ++index) {
		const auto base = index * mncwf_v4_lineage_entry_bytes;
		const auto &entry = entries[index];
		put_u16(records, base + 0,
			static_cast<std::uint16_t>(entry.relation));
		put_u16(records, base + 2, entry.flags);
		ensure_write(records, base + 8, entry.related_capture_uuid.size());
		std::copy(entry.related_capture_uuid.begin(),
			entry.related_capture_uuid.end(), records.begin() +
			static_cast<std::ptrdiff_t>(base + 8));
		std::copy(entry.related_event_uuid.begin(), entry.related_event_uuid.end(),
			records.begin() + static_cast<std::ptrdiff_t>(base + 24));
		put_u64(records, base + 40, entry.first_sequence);
		put_u64(records, base + 48, entry.last_sequence);
		put_u32(records, base + 56, entry.part_index);
		put_u32(records, base + 60, entry.part_count);
	}
	return enveloped_section(MncwfV4SectionType::lineage, entries.size(),
		mncwf_v4_lineage_entry_bytes, std::move(records));
}

} // namespace

std::vector<std::byte> encode_mncwf_v4(const MncwfV4Document &document)
{
	const auto sample_bytes = checked_multiply(document.sample_frame_count,
		document.sample_frame_bytes, "encoded sample data");
	if (document.sample_data.size() != sample_bytes)
		reject("encoder sample geometry is inconsistent");
	std::array<EncodedSection, mncwf_v4_mandatory_section_count> sections{
		encode_capture(document.capture_metadata),
		encode_timebase(document.timebase_segments),
		encode_channels(document.channels),
		encode_events(document.events),
		encode_quality(document.quality_intervals),
		encode_lineage(document.lineage),
		enveloped_section(MncwfV4SectionType::sample_data,
			document.sample_frame_count, document.sample_frame_bytes,
			document.sample_data),
	};

	const auto directory_bytes = checked_multiply(sections.size(),
		mncwf_v4_directory_entry_bytes, "encoded directory");
	auto next_offset = align_eight(checked_add(mncwf_v4_header_bytes,
		directory_bytes, "encoded directory"));
	std::vector<std::uint64_t> offsets;
	offsets.reserve(sections.size());
	for (const auto &section : sections) {
		offsets.push_back(next_offset);
		next_offset = align_eight(checked_add(next_offset,
			section.payload.size(), "encoded file"));
	}
	/* Section extents, including the final one, have zero eight-byte padding. */
	const auto file_bytes = next_offset;
	if (file_bytes > mncwf_v4_max_file_bytes)
		reject("encoded file exceeds the 512 MiB bound");
	EncodedBytes file(narrow_size(file_bytes, "encoded file"));
	std::copy(mncwf_magic.begin(), mncwf_magic.end(), file.begin());
	put_u32(file, 8, mncwf_v4_version);
	put_u32(file, 12, mncwf_v4_header_bytes);
	put_u32(file, 16, mncwf_v4_directory_entry_bytes);
	put_u32(file, 20, static_cast<std::uint32_t>(sections.size()));
	put_u64(file, 24, mncwf_v4_header_bytes);
	put_u64(file, 32, directory_bytes);
	put_u64(file, 40, file_bytes);

	for (std::size_t index = 0; index < sections.size(); ++index) {
		const auto directory = mncwf_v4_header_bytes +
			index * mncwf_v4_directory_entry_bytes;
		const auto &section = sections[index];
		put_u32(file, directory + 0,
			static_cast<std::uint32_t>(section.type));
		put_u16(file, directory + 4, 1u);
		put_u16(file, directory + 6, mncwf_v4_section_required);
		put_u64(file, directory + 8, offsets[index]);
		put_u64(file, directory + 16, section.payload.size());
		put_u64(file, directory + 24, section.payload.size());
		put_u64(file, directory + 32, section.item_count);
		put_u32(file, directory + 40, section.item_bytes);
		put_u32(file, directory + 44, mncwf_crc32c(section.payload));
		std::copy(section.payload.begin(), section.payload.end(), file.begin() +
			static_cast<std::ptrdiff_t>(offsets[index]));
	}
	const auto directory = std::span<const std::byte>{file}.subspan(
		mncwf_v4_header_bytes, narrow_size(directory_bytes, "directory"));
	put_u32(file, 52, mncwf_crc32c(directory));
	put_u32(file, 56, 0u);
	put_u32(file, 56, mncwf_crc32c(
		std::span<const std::byte>{file}.first(mncwf_v4_header_bytes)));

	/* One implementation is the writer; the other is the acceptance oracle. */
	(void)MncwfV4Reader{file};
	return file;
}

namespace {

std::uint64_t event_last_sequence(const MncwfV4EventDescriptor &event)
{
	if ((event.flags & mncwf_event_end_valid) != 0u)
		return event.end_sequence;
	if ((event.flags & mncwf_event_current_valid) != 0u)
		return event.current_sequence;
	return event.start_sequence;
}

MncwfUuid virtual_capture_uuid(const MncwfUuid &parent,
	const MncwfUuid &event, std::uint64_t first_frame,
	std::uint64_t frame_count)
{
	constexpr std::string_view domain = "MNCWF-v4 virtual event slice";
	EncodedBytes seed;
	const auto domain_bytes = std::as_bytes(
		std::span{domain.data(), domain.size()});
	seed.insert(seed.end(), domain_bytes.begin(), domain_bytes.end());
	seed.insert(seed.end(), parent.begin(), parent.end());
	seed.insert(seed.end(), event.begin(), event.end());
	const auto offset = seed.size();
	seed.resize(offset + 16u);
	put_u64(seed, offset, first_frame);
	put_u64(seed, offset + 8u, frame_count);
	const auto digest = mncwf_sha256(seed);
	MncwfUuid result{};
	std::copy_n(digest.begin(), result.size(), result.begin());
	/* Deterministic name-derived UUID, RFC-4122 variant/version-5 layout. */
	result[6] = static_cast<std::byte>(
		(std::to_integer<std::uint8_t>(result[6]) & 0x0fu) | 0x50u);
	result[8] = static_cast<std::byte>(
		(std::to_integer<std::uint8_t>(result[8]) & 0x3fu) | 0x80u);
	return result;
}

std::pair<std::uint64_t, std::uint64_t> frame_source_bounds(
	const std::vector<MncwfV4TimebaseSegment> &segments,
	std::uint64_t frame)
{
	const auto found = std::ranges::find_if(segments,
		[frame](const auto &segment) {
			return frame >= segment.first_frame &&
			       frame - segment.first_frame < segment.frame_count;
		});
	if (found == segments.end())
		reject("virtual slice frame has no timebase segment");
	const auto local = frame - found->first_frame;
	const auto first = checked_add(found->first_sequence,
		checked_multiply(local, found->decimation_divisor,
			"virtual frame sequence"),
		"virtual frame sequence");
	const auto segment_end = checked_add(found->first_sequence,
		found->source_frame_count, "virtual segment source range");
	const auto group_end = std::min(segment_end,
		checked_add(first, found->decimation_divisor,
			"virtual frame source range"));
	return {first, group_end - 1u};
}

const MncwfV4TimebaseSegment &segment_for_sequence(
	const std::vector<MncwfV4TimebaseSegment> &segments,
	std::uint64_t sequence)
{
	const auto found = std::ranges::find_if(segments,
		[sequence](const auto &segment) {
			return sequence >= segment.first_sequence &&
			       sequence - segment.first_sequence <
				       segment.source_frame_count;
		});
	if (found == segments.end())
		reject("virtual event anchor has no timebase segment");
	return *found;
}

std::uint64_t timestamp_for_sequence(
	const std::vector<MncwfV4TimebaseSegment> &segments,
	std::uint64_t sequence, bool utc)
{
	const auto &segment = segment_for_sequence(segments, sequence);
	const auto anchor = utc ? segment.correlation_utc_nanoseconds
		: segment.correlation_tai_nanoseconds;
	if (anchor == 0u)
		reject(utc ? "virtual event lacks a UTC timebase" :
			"virtual event lacks a TAI timebase");
	const auto delta = sequence >= segment.correlation_sequence
		? sequence - segment.correlation_sequence
		: segment.correlation_sequence - sequence;
	const auto scaled = checked_multiply(delta,
		segment.acquisition_rate_denominator,
		"virtual event timestamp delta");
	const auto nanoseconds = checked_multiply(scaled, 1'000'000'000ull,
		"virtual event timestamp delta") /
		segment.acquisition_rate_numerator;
	if (sequence >= segment.correlation_sequence) {
		return checked_add(anchor, nanoseconds,
			"virtual event timestamp");
	}
	if (nanoseconds > anchor)
		reject("virtual event timestamp underflows");
	return anchor - nanoseconds;
}

void restamp_event(MncwfV4EventDescriptor &event,
	const MncwfV4EventDescriptor &original,
	const std::vector<MncwfV4TimebaseSegment> &segments)
{
	const auto stamp = [&](std::uint64_t sequence,
		std::uint64_t original_sequence, std::uint64_t &tai,
		std::uint64_t &utc) {
		if ((event.flags & mncwf_event_tai_valid) == 0u)
			tai = 0u;
		else if (sequence != original_sequence)
			tai = timestamp_for_sequence(segments, sequence, false);
		if ((event.flags & mncwf_event_utc_valid) == 0u)
			utc = 0u;
		else if (sequence != original_sequence)
			utc = timestamp_for_sequence(segments, sequence, true);
	};
	stamp(event.start_sequence, original.start_sequence,
		event.start_tai_nanoseconds, event.start_utc_nanoseconds);
	if ((event.flags & mncwf_event_current_valid) != 0u)
		stamp(event.current_sequence, original.current_sequence,
			event.current_tai_nanoseconds,
			event.current_utc_nanoseconds);
	else {
		event.current_tai_nanoseconds = 0u;
		event.current_utc_nanoseconds = 0u;
	}
	if ((event.flags & mncwf_event_end_valid) != 0u)
		stamp(event.end_sequence, original.end_sequence,
			event.end_tai_nanoseconds, event.end_utc_nanoseconds);
	else {
		event.end_tai_nanoseconds = 0u;
		event.end_utc_nanoseconds = 0u;
	}
	if ((event.flags & mncwf_event_trigger_valid) != 0u)
		stamp(event.trigger_sequence, original.trigger_sequence,
			event.trigger_tai_nanoseconds,
			event.trigger_utc_nanoseconds);
	else {
		event.trigger_tai_nanoseconds = 0u;
		event.trigger_utc_nanoseconds = 0u;
	}
}

void write_directory_entry(EncodedBytes &file, std::size_t index,
	MncwfV4SectionType type, std::uint64_t offset,
	std::uint64_t stored_bytes, std::uint64_t item_count,
	std::uint32_t item_bytes, std::uint32_t crc)
{
	const auto directory = mncwf_v4_header_bytes +
		index * mncwf_v4_directory_entry_bytes;
	put_u32(file, directory + 0u, static_cast<std::uint32_t>(type));
	put_u16(file, directory + 4u, 1u);
	put_u16(file, directory + 6u, mncwf_v4_section_required);
	put_u64(file, directory + 8u, offset);
	put_u64(file, directory + 16u, stored_bytes);
	put_u64(file, directory + 24u, stored_bytes);
	put_u64(file, directory + 32u, item_count);
	put_u32(file, directory + 40u, item_bytes);
	put_u32(file, directory + 44u, crc);
}

} // namespace

MncwfV4VirtualFile make_mncwf_v4_event_slice(
	const MncwfV4Reader &reader, const MncwfUuid &event_uuid)
{
	if (all_zero(event_uuid))
		reject("virtual slice event UUID is zero");
	const auto selected = std::ranges::find_if(reader.events(),
		[&event_uuid](const auto &event) {
			return event.event_uuid == event_uuid;
		});
	if (selected == reader.events().end())
		reject("virtual slice event UUID is not in the capture");

	const auto requested_first = selected->start_sequence;
	const auto requested_last = event_last_sequence(*selected);
	std::optional<std::uint64_t> first_frame;
	std::uint64_t last_frame_exclusive = 0u;
	for (const auto &segment : reader.timebase_segments()) {
		const auto segment_last = checked_add(segment.first_sequence,
			segment.source_frame_count - 1u,
			"virtual segment sequence range");
		if (requested_last < segment.first_sequence ||
		    requested_first > segment_last)
			continue;
		const auto local_first = requested_first <= segment.first_sequence
			? 0u : (requested_first - segment.first_sequence) /
				segment.decimation_divisor;
		const auto local_last = requested_last >= segment_last
			? segment.frame_count - 1u
			: (requested_last - segment.first_sequence) /
				segment.decimation_divisor;
		const auto absolute_first = checked_add(segment.first_frame,
			local_first, "virtual first frame");
		const auto absolute_last = checked_add(segment.first_frame,
			local_last + 1u, "virtual last frame");
		first_frame = first_frame ? std::min(*first_frame, absolute_first)
			: absolute_first;
		last_frame_exclusive = std::max(last_frame_exclusive, absolute_last);
	}
	if (!first_frame || last_frame_exclusive <= *first_frame)
		reject("virtual slice event has no sample frames");
	const auto frame_count = last_frame_exclusive - *first_frame;

	std::vector<MncwfV4TimebaseSegment> timebase;
	for (const auto &source : reader.timebase_segments()) {
		const auto source_end = checked_add(source.first_frame,
			source.frame_count, "virtual source frame range");
		const auto begin = std::max(source.first_frame, *first_frame);
		const auto end = std::min(source_end, last_frame_exclusive);
		if (begin >= end)
			continue;
		auto segment = source;
		const auto local_first = begin - source.first_frame;
		const auto local_end = end - source.first_frame;
		segment.first_frame = begin - *first_frame;
		segment.frame_count = end - begin;
		segment.first_sequence = checked_add(source.first_sequence,
			checked_multiply(local_first, source.decimation_divisor,
				"virtual segment first sequence"),
			"virtual segment first sequence");
		const auto original_end = checked_add(source.first_sequence,
			source.source_frame_count, "virtual source sequence range");
		const auto selected_end = std::min(original_end,
			checked_add(source.first_sequence,
				checked_multiply(local_end, source.decimation_divisor,
					"virtual segment source count"),
				"virtual segment source count"));
		segment.source_frame_count = selected_end - segment.first_sequence;
		if (timebase.empty())
			segment.flags &= static_cast<std::uint16_t>(
				~(mncwf_time_rate_change_before |
					mncwf_time_sequence_gap_before));
		timebase.push_back(segment);
	}
	if (timebase.empty())
		reject("virtual slice has no timebase segments");
	const auto slice_first_sequence = timebase.front().first_sequence;
	const auto slice_last_sequence = checked_add(
		timebase.back().first_sequence,
		timebase.back().source_frame_count - 1u,
		"virtual source sequence range");

	std::vector<MncwfV4EventDescriptor> events;
	for (const auto &source : reader.events()) {
		if (event_last_sequence(source) < slice_first_sequence ||
		    source.start_sequence > slice_last_sequence)
			continue;
		auto event = source;
		const auto clamp = [slice_first_sequence, slice_last_sequence](
			std::uint64_t value) {
			return std::clamp(value, slice_first_sequence,
				slice_last_sequence);
		};
		const bool clipped_start = event.start_sequence < slice_first_sequence;
		const bool clipped_current =
			(event.flags & mncwf_event_current_valid) != 0u &&
			event.current_sequence > slice_last_sequence;
		const bool clipped_end =
			(event.flags & mncwf_event_end_valid) != 0u &&
			event.end_sequence > slice_last_sequence;
		event.start_sequence = clamp(event.start_sequence);
		if ((event.flags & mncwf_event_current_valid) != 0u)
			event.current_sequence = std::max(event.start_sequence,
				clamp(event.current_sequence));
		if (clipped_end) {
			event.flags &= ~mncwf_event_end_valid;
			event.end_sequence = 0u;
		} else if ((event.flags & mncwf_event_end_valid) != 0u) {
			event.end_sequence = std::max(event.current_sequence,
				clamp(event.end_sequence));
		}
		if ((event.flags & mncwf_event_trigger_valid) != 0u &&
		    (event.trigger_sequence < slice_first_sequence ||
		     event.trigger_sequence > slice_last_sequence)) {
			event.flags &= ~mncwf_event_trigger_valid;
			event.trigger_sequence = 0u;
		}
		if (clipped_start || clipped_current || clipped_end) {
			event.flags |= mncwf_event_discontinuous;
			if ((event.flags & mncwf_event_end_valid) == 0u)
				event.lifecycle = MncwfEventLifecycle::update;
		}
		if ((event.lifecycle == MncwfEventLifecycle::end ||
		     event.lifecycle == MncwfEventLifecycle::abort ||
		     event.lifecycle == MncwfEventLifecycle::complete) &&
		    (event.flags & mncwf_event_end_valid) == 0u)
			event.lifecycle = MncwfEventLifecycle::update;
		event.duration_samples = event_last_sequence(event) -
			event.start_sequence;
		restamp_event(event, source, timebase);
		events.push_back(std::move(event));
	}
	if (std::ranges::find_if(events, [&event_uuid](const auto &event) {
		return event.event_uuid == event_uuid;
	}) == events.end())
		reject("virtual slice lost its selected event");

	std::vector<MncwfV4QualityInterval> quality;
	for (const auto &source : reader.quality_intervals()) {
		const auto source_end = checked_add(source.first_frame,
			source.frame_count, "virtual quality frame range");
		const auto begin = std::max(source.first_frame, *first_frame);
		const auto end = std::min(source_end, last_frame_exclusive);
		if (begin >= end)
			continue;
		auto interval = source;
		interval.first_frame = begin - *first_frame;
		interval.frame_count = end - begin;
		const auto first_bounds = frame_source_bounds(
			reader.timebase_segments(), begin);
		const auto last_bounds = frame_source_bounds(
			reader.timebase_segments(), end - 1u);
		interval.first_sequence = std::max(source.first_sequence,
			first_bounds.first);
		interval.last_sequence = std::min(source.last_sequence,
			last_bounds.second);
		if (interval.first_sequence <= interval.last_sequence)
			quality.push_back(interval);
	}

	auto capture = reader.capture_metadata();
	const auto parent_uuid = capture.capture_uuid;
	capture.capture_uuid = virtual_capture_uuid(parent_uuid, event_uuid,
		*first_frame, frame_count);
	if (!capture.comments.empty())
		capture.comments += ";";
	capture.comments += "virtual_event_slice";

	std::vector<MncwfV4LineageEntry> lineage;
	for (const auto &source : reader.lineage()) {
		if (source.relation == MncwfLineageRelation::event ||
		    source.last_sequence < slice_first_sequence ||
		    source.first_sequence > slice_last_sequence)
			continue;
		auto entry = source;
		entry.first_sequence = std::max(entry.first_sequence,
			slice_first_sequence);
		entry.last_sequence = std::min(entry.last_sequence,
			slice_last_sequence);
		lineage.push_back(entry);
	}
	lineage.push_back({MncwfLineageRelation::parent, 0u, parent_uuid, {},
		slice_first_sequence, slice_last_sequence, 0u, 1u});
	lineage.push_back({MncwfLineageRelation::virtual_slice, 0u, parent_uuid,
		event_uuid, slice_first_sequence, slice_last_sequence, 0u, 1u});
	for (const auto &event : events)
		lineage.push_back({MncwfLineageRelation::event, 0u,
			capture.capture_uuid, event.event_uuid, event.start_sequence,
			event_last_sequence(event), 0u, 1u});
	if (lineage.size() > mncwf_v4_max_lineage_entries)
		reject("virtual slice lineage exceeds its bound");

	const auto sample_offset = checked_multiply(*first_frame,
		reader.sample_frame_bytes(), "virtual sample offset");
	const auto sample_bytes = checked_multiply(frame_count,
		reader.sample_frame_bytes(), "virtual sample bytes");
	const auto sample_data = reader.sample_data().subspan(
		narrow_size(sample_offset, "virtual sample offset"),
		narrow_size(sample_bytes, "virtual sample bytes"));

	std::array<EncodedSection, 6> metadata{
		encode_capture(capture), encode_timebase(timebase),
		encode_channels(reader.channels()), encode_events(events),
		encode_quality(quality), encode_lineage(lineage),
	};
	constexpr auto section_count = mncwf_v4_mandatory_section_count;
	const auto directory_bytes = checked_multiply(section_count,
		mncwf_v4_directory_entry_bytes, "virtual directory");
	auto next_offset = align_eight(checked_add(mncwf_v4_header_bytes,
		directory_bytes, "virtual directory"));
	std::array<std::uint64_t, section_count> offsets{};
	for (std::size_t index = 0; index < metadata.size(); ++index) {
		offsets[index] = next_offset;
		next_offset = align_eight(checked_add(next_offset,
			metadata[index].payload.size(), "virtual metadata"));
	}
	offsets.back() = next_offset;
	const auto sample_stored_bytes = checked_add(
		mncwf_v4_section_header_bytes, sample_data.size(),
		"virtual sample section");
	const auto file_bytes = align_eight(checked_add(offsets.back(),
		sample_stored_bytes, "virtual file"));
	if (file_bytes > mncwf_v4_max_file_bytes)
		reject("virtual file exceeds the 512 MiB bound");
	const auto prefix_bytes = checked_add(offsets.back(),
		mncwf_v4_section_header_bytes, "virtual prefix");
	EncodedBytes prefix(narrow_size(prefix_bytes, "virtual prefix"));
	std::copy(mncwf_magic.begin(), mncwf_magic.end(), prefix.begin());
	put_u32(prefix, 8u, mncwf_v4_version);
	put_u32(prefix, 12u, mncwf_v4_header_bytes);
	put_u32(prefix, 16u, mncwf_v4_directory_entry_bytes);
	put_u32(prefix, 20u, section_count);
	put_u64(prefix, 24u, mncwf_v4_header_bytes);
	put_u64(prefix, 32u, directory_bytes);
	put_u64(prefix, 40u, file_bytes);
	for (std::size_t index = 0; index < metadata.size(); ++index) {
		const auto &section = metadata[index];
		write_directory_entry(prefix, index, section.type, offsets[index],
			section.payload.size(), section.item_count, section.item_bytes,
			mncwf_crc32c(section.payload));
		std::copy(section.payload.begin(), section.payload.end(),
			prefix.begin() + static_cast<std::ptrdiff_t>(offsets[index]));
	}
	EncodedBytes sample_header(mncwf_v4_section_header_bytes);
	put_u32(sample_header, 0u,
		static_cast<std::uint32_t>(MncwfV4SectionType::sample_data));
	put_u16(sample_header, 4u, 1u);
	put_u16(sample_header, 6u, mncwf_v4_section_header_bytes);
	put_u32(sample_header, 12u, reader.sample_frame_bytes());
	put_u64(sample_header, 16u, frame_count);
	std::copy(sample_header.begin(), sample_header.end(),
		prefix.begin() + static_cast<std::ptrdiff_t>(offsets.back()));
	auto sample_crc = crc32c_update(0xffffffffu, sample_header);
	sample_crc = crc32c_update(sample_crc, sample_data) ^ 0xffffffffu;
	write_directory_entry(prefix, metadata.size(),
		MncwfV4SectionType::sample_data, offsets.back(), sample_stored_bytes,
		frame_count, reader.sample_frame_bytes(), sample_crc);
	const auto directory = std::span<const std::byte>{prefix}.subspan(
		mncwf_v4_header_bytes, narrow_size(directory_bytes, "virtual directory"));
	put_u32(prefix, 52u, mncwf_crc32c(directory));
	put_u32(prefix, 56u, 0u);
	put_u32(prefix, 56u, mncwf_crc32c(
		std::span<const std::byte>{prefix}.first(mncwf_v4_header_bytes)));

	MncwfV4VirtualFile result{};
	result.prefix_ = std::move(prefix);
	result.sample_data_ = sample_data;
	result.file_bytes_ = file_bytes;
	result.trailing_padding_bytes_ = static_cast<std::uint8_t>(
		file_bytes - prefix_bytes - sample_data.size());
	result.capture_uuid_ = capture.capture_uuid;
	result.first_sequence_ = slice_first_sequence;
	result.last_sequence_ = slice_last_sequence;
	return result;
}

std::size_t MncwfV4VirtualFile::read(std::uint64_t offset,
	std::span<std::byte> destination) const noexcept
{
	if (offset >= file_bytes_ || destination.empty())
		return 0u;
	const auto requested = static_cast<std::size_t>(std::min<std::uint64_t>(
		destination.size(), file_bytes_ - offset));
	std::size_t copied = 0u;
	const auto copy_region = [&](std::uint64_t region_offset,
		std::span<const std::byte> source) {
		if (copied == requested || offset + copied < region_offset ||
		    offset + copied >= region_offset + source.size())
			return;
		const auto source_offset = static_cast<std::size_t>(
			offset + copied - region_offset);
		const auto count = std::min(requested - copied,
			source.size() - source_offset);
		std::copy_n(source.begin() + static_cast<std::ptrdiff_t>(source_offset),
			count, destination.begin() + static_cast<std::ptrdiff_t>(copied));
		copied += count;
	};
	copy_region(0u, prefix_);
	copy_region(prefix_.size(), sample_data_);
	if (copied < requested) {
		const auto padding_offset = prefix_.size() + sample_data_.size();
		if (offset + copied >= padding_offset) {
			const auto count = std::min<std::size_t>(requested - copied,
				trailing_padding_bytes_ - static_cast<std::size_t>(
					offset + copied - padding_offset));
			std::fill_n(destination.begin() +
				static_cast<std::ptrdiff_t>(copied), count, std::byte{0});
			copied += count;
		}
	}
	return copied;
}

MncwfV4Reader::MncwfV4Reader(std::span<const std::byte> bytes) : bytes_(bytes)
{
	if (bytes.size() < mncwf_v4_header_bytes)
		reject("header is truncated");
	if (bytes.size() > mncwf_v4_max_file_bytes)
		reject("file exceeds the configured size bound");
	if (!std::equal(mncwf_magic.begin(), mncwf_magic.end(), bytes.begin()))
		reject("magic is invalid");
	if (read_u32(bytes, 8) != mncwf_v4_version)
		reject("version is not 4");
	if (read_u32(bytes, 12) != mncwf_v4_header_bytes ||
	    read_u32(bytes, 16) != mncwf_v4_directory_entry_bytes)
		reject("header geometry is unsupported");

	header_.section_count = read_u32(bytes, 20);
	header_.directory_offset = read_u64(bytes, 24);
	header_.directory_bytes = read_u64(bytes, 32);
	header_.file_bytes = read_u64(bytes, 40);
	header_.flags = read_u32(bytes, 48);
	const auto directory_crc = read_u32(bytes, 52);
	const auto header_crc = read_u32(bytes, 56);
	if (read_u32(bytes, 60) != 0u || header_.flags != 0u)
		reject("header reserved fields are nonzero");
	if (header_.section_count < mncwf_v4_mandatory_section_count ||
	    header_.section_count > mncwf_v4_max_sections)
		reject("section count is outside its bound");
	if (header_.directory_offset != mncwf_v4_header_bytes ||
	    header_.directory_bytes !=
		    checked_multiply(header_.section_count,
			mncwf_v4_directory_entry_bytes, "directory") ||
	    header_.file_bytes != bytes.size())
		reject("directory or file geometry is invalid");

	std::array<std::byte, mncwf_v4_header_bytes> header_copy{};
	std::copy_n(bytes.begin(), mncwf_v4_header_bytes, header_copy.begin());
	std::fill(header_copy.begin() + 56, header_copy.begin() + 60,
		std::byte{0});
	if (mncwf_crc32c(header_copy) != header_crc)
		reject("header CRC32C mismatch");
	const auto directory = range(bytes, header_.directory_offset,
		header_.directory_bytes, "directory");
	if (mncwf_crc32c(directory) != directory_crc)
		reject("directory CRC32C mismatch");

	sections_.reserve(header_.section_count);
	std::array<bool, mncwf_v4_mandatory_section_count> present{};
	for (std::uint32_t index = 0; index < header_.section_count; ++index) {
		const auto offset = static_cast<std::size_t>(index) *
			mncwf_v4_directory_entry_bytes;
		MncwfV4SectionInfo section{};
		section.type = read_u32(directory, offset);
		section.version = read_u16(directory, offset + 4u);
		section.flags = read_u16(directory, offset + 6u);
		section.offset = read_u64(directory, offset + 8u);
		section.stored_bytes = read_u64(directory, offset + 16u);
		section.logical_bytes = read_u64(directory, offset + 24u);
		section.item_count = read_u64(directory, offset + 32u);
		section.item_bytes = read_u32(directory, offset + 40u);
		section.crc32c = read_u32(directory, offset + 44u);
		if (read_u64(directory, offset + 48u) != 0u)
			reject("directory reserved field is nonzero");
		if (section.type == 0u || section.version == 0u ||
		    (section.flags & ~mncwf_v4_known_section_flags) != 0u ||
		    section.stored_bytes == 0u ||
		    section.item_count > mncwf_v4_max_file_bytes ||
		    section.logical_bytes != section.stored_bytes ||
		    (section.offset & 7u) != 0u)
			reject("directory entry is invalid");
		if (!known_section(section.type) &&
		    (section.flags & mncwf_v4_section_required) != 0u)
			reject("unknown required section");
		if (known_section(section.type)) {
			const auto slot = section_index(section.type);
			if (present[slot])
				reject("duplicate mandatory section");
			present[slot] = true;
			if (section.flags != mncwf_v4_section_required)
				reject("known section is not mandatory");
			if (section.type != static_cast<std::uint32_t>(
					MncwfV4SectionType::sample_data) &&
			    section.stored_bytes >
				    mncwf_v4_max_metadata_section_bytes)
				reject("metadata section exceeds its size bound");
		}
		section.data = range(bytes, section.offset, section.stored_bytes,
			"section");
		sections_.push_back(section);
	}
	if (!std::ranges::all_of(present, [](bool value) { return value; }))
		reject("mandatory section is missing");

	std::vector<std::pair<std::uint64_t, std::uint64_t>> extents;
	extents.reserve(sections_.size());
	for (const auto &section : sections_)
		extents.emplace_back(section.offset,
			checked_add(section.offset, section.stored_bytes,
				"section extent"));
	std::ranges::sort(extents);
	auto cursor = align_eight(checked_add(header_.directory_offset,
		header_.directory_bytes, "directory extent"));
	for (const auto &[begin, end] : extents) {
		if (begin != cursor)
			reject(begin < cursor ? "sections overlap" :
				"unreferenced bytes appear between sections");
		cursor = align_eight(end);
		if (cursor > bytes.size())
			reject("final section alignment exceeds the file");
		if (end != cursor) {
			const auto padding = range(bytes, end, cursor - end,
				"section alignment padding");
			if (!all_zero(padding))
				reject("section alignment padding is nonzero");
		}
	}
	if (cursor != bytes.size())
		reject("unreferenced bytes follow the final section");
	for (const auto &section : sections_)
		if (mncwf_crc32c(section.data) != section.crc32c)
			reject("section CRC32C mismatch");

	const auto find_section = [this](MncwfV4SectionType type)
			-> const MncwfV4SectionInfo & {
		const auto raw = static_cast<std::uint32_t>(type);
		const auto found = std::ranges::find_if(sections_,
			[raw](const auto &section) { return section.type == raw; });
		if (found == sections_.end())
			reject("mandatory section lookup failed");
		return *found;
	};

	const auto capture_section = parse_section_envelope(
		find_section(MncwfV4SectionType::capture_metadata),
		mncwf_v4_capture_metadata_bytes, 1u, 1u, true);
	const auto capture = capture_section.records;
	capture_metadata_.capture_uuid = read_array<16>(capture, 0);
	capture_metadata_.device_uuid = read_array<16>(capture, 16);
	capture_metadata_.configuration_sha256 = read_array<32>(capture, 32);
	capture_metadata_.sensor_profile_sha256 = read_array<32>(capture, 64);
	capture_metadata_.created_tai_nanoseconds = read_u64(capture, 96);
	capture_metadata_.created_utc_nanoseconds = read_u64(capture, 104);
	capture_metadata_.nominal_voltage_numerator = read_s64(capture, 112);
	capture_metadata_.nominal_voltage_denominator = read_u64(capture, 120);
	capture_metadata_.nominal_frequency_numerator = read_u64(capture, 128);
	capture_metadata_.nominal_frequency_denominator = read_u64(capture, 136);
	const auto topology = read_u32(capture, 144);
	const auto calibration = read_u32(capture, 148);
	capture_metadata_.flags = read_u32(capture, 152);
	if (all_zero(capture_metadata_.capture_uuid) ||
	    all_zero(capture_metadata_.device_uuid) ||
	    all_zero(capture_metadata_.configuration_sha256) ||
	    all_zero(capture_metadata_.sensor_profile_sha256) ||
	    capture_metadata_.nominal_voltage_denominator == 0u ||
	    capture_metadata_.nominal_frequency_denominator == 0u ||
	    !valid_topology(topology) || !valid_calibration(calibration) ||
	    capture_metadata_.flags != 0u || read_u32(capture, 156) != 0u)
		reject("capture metadata is invalid");
	capture_metadata_.topology = static_cast<MncwfTopology>(topology);
	capture_metadata_.calibration_status =
		static_cast<MncwfCalibrationStatus>(calibration);
	std::array<std::string *, 12> capture_strings{
		&capture_metadata_.station_name,
		&capture_metadata_.site_name,
		&capture_metadata_.circuit_name,
		&capture_metadata_.product_name,
		&capture_metadata_.device_model,
		&capture_metadata_.firmware_version,
		&capture_metadata_.software_build_id,
		&capture_metadata_.sensor_profile_id,
		&capture_metadata_.configuration_id,
		&capture_metadata_.calibration_id,
		&capture_metadata_.device_serial,
		&capture_metadata_.comments,
	};
	for (std::size_t index = 0; index < capture_strings.size(); ++index)
		*capture_strings[index] = read_string(capture, 160u + index * 8u,
			capture_section.blob, "capture metadata string");
	if (capture_metadata_.product_name.empty() ||
	    capture_metadata_.firmware_version.empty() ||
	    capture_metadata_.software_build_id.empty() ||
	    capture_metadata_.sensor_profile_id.empty() ||
	    capture_metadata_.configuration_id.empty())
		reject("required capture identity is empty");

	const auto time_section = parse_section_envelope(
		find_section(MncwfV4SectionType::timebase_segments),
		mncwf_v4_timebase_segment_bytes, 1u, 65536u, false);
	timebase_segments_.reserve(narrow_size(
		find_section(MncwfV4SectionType::timebase_segments).item_count,
		"timebase segment count"));
	std::uint64_t expected_first_frame = 0;
	std::optional<std::uint64_t> expected_next_sequence;
	for (std::uint64_t index = 0;
	     index < find_section(MncwfV4SectionType::timebase_segments).item_count;
	     ++index) {
		const auto record = time_section.records.subspan(
			narrow_size(index * mncwf_v4_timebase_segment_bytes,
				"timebase record"),
			mncwf_v4_timebase_segment_bytes);
		MncwfV4TimebaseSegment segment{};
		segment.first_frame = read_u64(record, 0);
		segment.frame_count = read_u64(record, 8);
		segment.first_sequence = read_u64(record, 16);
		segment.sequence_step = read_u64(record, 24);
		segment.acquisition_rate_numerator = read_u64(record, 32);
		segment.acquisition_rate_denominator = read_u64(record, 40);
		segment.persisted_rate_numerator = read_u64(record, 48);
		segment.persisted_rate_denominator = read_u64(record, 56);
		segment.correlation_sequence = read_u64(record, 64);
		segment.correlation_pl_tick = read_u64(record, 72);
		segment.correlation_tai_nanoseconds = read_u64(record, 80);
		segment.correlation_utc_nanoseconds = read_u64(record, 88);
		segment.uncertainty_nanoseconds = read_u64(record, 96);
		segment.decimation_divisor = read_u32(record, 104);
		const auto method = read_u16(record, 108);
		const auto clock = read_u16(record, 110);
		const auto quality = read_u16(record, 112);
		segment.flags = read_u16(record, 114);
		segment.utc_offset_seconds = read_s32(record, 116);
		segment.source_frame_count = read_u64(record, 120);
		if (segment.first_frame != expected_first_frame ||
		    segment.frame_count == 0u || segment.sequence_step == 0u ||
		    segment.decimation_divisor == 0u ||
		    segment.decimation_divisor > 65536u ||
		    (segment.flags & ~mncwf_time_known_flags) != 0u ||
		    !valid_clock_source(clock) || !valid_time_quality(quality) ||
		    method > static_cast<std::uint16_t>(
				     MncwfDecimationMethod::boxcar_mean_toward_zero) ||
		    ((segment.flags & mncwf_time_utc_offset_known) == 0u &&
		     (segment.utc_offset_seconds != 0 ||
		      segment.correlation_utc_nanoseconds != 0u ||
		      (segment.flags & (mncwf_time_positive_leap_pending |
				mncwf_time_negative_leap_pending)) != 0u)) ||
		    segment.correlation_tai_nanoseconds == 0u ||
		    ((segment.flags & mncwf_time_positive_leap_pending) != 0u &&
		     (segment.flags & mncwf_time_negative_leap_pending) != 0u))
			reject("timebase segment is invalid");
		segment.decimation_method =
			static_cast<MncwfDecimationMethod>(method);
		segment.clock_source = static_cast<MncwfClockSource>(clock);
		segment.time_quality = static_cast<MncwfTimeQuality>(quality);
		if (segment.decimation_method == MncwfDecimationMethod::none) {
			if (segment.decimation_divisor != 1u ||
			    segment.sequence_step != 1u ||
			    segment.source_frame_count != segment.frame_count)
				reject("non-decimated timebase geometry is inconsistent");
		} else {
			if (segment.sequence_step != segment.decimation_divisor)
				reject("boxcar sequence step does not equal its divisor");
			const auto minimum_source = checked_add(
				checked_multiply(segment.frame_count - 1u,
					segment.decimation_divisor,
					"boxcar source-frame minimum"),
				1u, "boxcar source-frame minimum");
			const auto maximum_source = checked_multiply(segment.frame_count,
				segment.decimation_divisor,
				"boxcar source-frame maximum");
			if (segment.source_frame_count < minimum_source ||
			    segment.source_frame_count > maximum_source)
				reject("boxcar source-frame count is inconsistent");
		}
		if (!rate_matches_decimation(segment))
			reject("persisted rate does not match acquisition rate/divisor");
		if (index == 0u &&
		    (segment.flags & (mncwf_time_rate_change_before |
				      mncwf_time_sequence_gap_before)) != 0u)
			reject("first timebase segment declares a preceding discontinuity");
		if (expected_next_sequence) {
			const bool gap =
				(segment.flags & mncwf_time_sequence_gap_before) != 0u;
			if ((!gap && segment.first_sequence != *expected_next_sequence) ||
			    (gap && segment.first_sequence <= *expected_next_sequence))
				reject("timebase sequence continuity is inconsistent");
		}
		if (index != 0u) {
			const auto &previous = timebase_segments_.back();
			const bool rate_changed =
				reduced(previous.acquisition_rate_numerator,
					previous.acquisition_rate_denominator) !=
					reduced(segment.acquisition_rate_numerator,
						segment.acquisition_rate_denominator) ||
				reduced(previous.persisted_rate_numerator,
					previous.persisted_rate_denominator) !=
					reduced(segment.persisted_rate_numerator,
						segment.persisted_rate_denominator) ||
				previous.decimation_divisor != segment.decimation_divisor ||
				previous.decimation_method != segment.decimation_method;
			const bool declared =
				(segment.flags & mncwf_time_rate_change_before) != 0u;
			if (rate_changed != declared)
				reject("timebase rate-change flag is inconsistent");
		}
		expected_next_sequence = checked_add(segment.first_sequence,
			segment.source_frame_count, "timebase source sequence");
		expected_first_frame = checked_add(segment.first_frame,
			segment.frame_count, "timebase frame coverage");
		timebase_segments_.push_back(segment);
	}

	const auto channel_info =
		find_section(MncwfV4SectionType::channel_definitions);
	const auto channel_section = parse_section_envelope(channel_info,
		mncwf_v4_channel_definition_bytes, 1u, mncwf_v4_max_channels, true);
	channels_.reserve(narrow_size(channel_info.item_count, "channel count"));
	std::set<MncwfUuid> channel_ids;
	std::set<std::uint32_t> source_channels;
	std::uint64_t expected_frame_bytes = 0;
	for (std::uint64_t index = 0; index < channel_info.item_count; ++index) {
		const auto record = channel_section.records.subspan(
			narrow_size(index * mncwf_v4_channel_definition_bytes,
				"channel record"),
			mncwf_v4_channel_definition_bytes);
		MncwfV4ChannelDefinition channel{};
		channel.stable_id = read_array<16>(record, 0);
		channel.source_channel = read_u32(record, 16);
		channel.flags = read_u32(record, 20);
		const auto phase = read_u16(record, 24);
		const auto quantity = read_u16(record, 26);
		const auto unit = read_u16(record, 28);
		const auto encoding = read_u16(record, 30);
		channel.storage_bits = read_u16(record, 32);
		channel.valid_bits = read_u16(record, 34);
		channel.display_exponent10 = read_s16(record, 36);
		channel.gain_numerator = read_s64(record, 40);
		channel.gain_denominator = read_u64(record, 48);
		channel.offset_numerator = read_s64(record, 56);
		channel.offset_denominator = read_u64(record, 64);
		channel.primary_secondary_ratio_numerator = read_u64(record, 72);
		channel.primary_secondary_ratio_denominator = read_u64(record, 80);
		channel.nominal_numerator = read_s64(record, 88);
		channel.nominal_denominator = read_u64(record, 96);
		channel.range_minimum_numerator = read_s64(record, 104);
		channel.range_minimum_denominator = read_u64(record, 112);
		channel.range_maximum_numerator = read_s64(record, 120);
		channel.range_maximum_denominator = read_u64(record, 128);
		channel.resolution_numerator = read_u64(record, 136);
		channel.resolution_denominator = read_u64(record, 144);
		channel.clipping_low = read_s64(record, 152);
		channel.clipping_high = read_s64(record, 160);
		channel.name = read_string(record, 168, channel_section.blob,
			"channel name");
		channel.unit_symbol = read_string(record, 176, channel_section.blob,
			"channel unit");
		channel.description = read_string(record, 184, channel_section.blob,
			"channel description");
		if (all_zero(channel.stable_id) ||
		    !channel_ids.insert(channel.stable_id).second ||
		    !source_channels.insert(channel.source_channel).second ||
		    (channel.flags & ~mncwf_channel_known_flags) != 0u ||
		    (channel.flags & mncwf_channel_enabled) == 0u ||
		    !valid_phase(phase) || !valid_quantity(quantity) ||
		    !valid_unit(unit) ||
		    encoding != static_cast<std::uint16_t>(
				MncwfSampleEncoding::signed_integer_little_endian) ||
		    channel.storage_bits == 0u || channel.storage_bits > 64u ||
		    (channel.storage_bits % 8u) != 0u || channel.valid_bits == 0u ||
		    channel.valid_bits > channel.storage_bits || read_u16(record, 38) != 0u ||
		    !all_zero(record.subspan(192, 16)) || channel.name.empty() ||
		    channel.unit_symbol.empty())
			reject("channel definition is invalid");
		if ((channel.flags & mncwf_channel_transform_valid) != 0u &&
		    (channel.gain_numerator == 0 || channel.gain_denominator == 0u ||
		     channel.offset_denominator == 0u))
			reject("channel affine transform is invalid");
		if ((channel.flags & mncwf_channel_ratio_valid) != 0u &&
		    (channel.primary_secondary_ratio_numerator == 0u ||
		     channel.primary_secondary_ratio_denominator == 0u))
			reject("channel ratio is invalid");
		if ((channel.flags & mncwf_channel_nominal_valid) != 0u &&
		    channel.nominal_denominator == 0u)
			reject("channel nominal has a zero denominator");
		if ((channel.flags & mncwf_channel_range_valid) != 0u &&
		    (channel.range_minimum_denominator == 0u ||
		     channel.range_maximum_denominator == 0u))
			reject("channel range has a zero denominator");
		if ((channel.flags & mncwf_channel_resolution_valid) != 0u &&
		    (channel.resolution_numerator == 0u ||
		     channel.resolution_denominator == 0u))
			reject("channel resolution is invalid");
		if ((channel.flags & mncwf_channel_clipping_valid) != 0u &&
		    channel.clipping_low > channel.clipping_high)
			reject("channel clipping bounds are reversed");
		channel.phase = static_cast<MncwfPhase>(phase);
		channel.quantity = static_cast<MncwfQuantity>(quantity);
		channel.si_unit = static_cast<MncwfSiUnit>(unit);
		channel.sample_encoding = static_cast<MncwfSampleEncoding>(encoding);
		expected_frame_bytes = checked_add(expected_frame_bytes,
			channel.storage_bits / 8u, "sample frame bytes");
		channels_.push_back(std::move(channel));
	}

	const auto event_info = find_section(MncwfV4SectionType::event_descriptors);
	const auto event_section = parse_section_envelope(event_info,
		mncwf_v4_event_descriptor_bytes, 0u, mncwf_v4_max_events, true);
	events_.reserve(narrow_size(event_info.item_count, "event count"));
	std::set<MncwfUuid> event_ids;
	for (std::uint64_t index = 0; index < event_info.item_count; ++index) {
		const auto record = event_section.records.subspan(
			narrow_size(index * mncwf_v4_event_descriptor_bytes,
				"event record"),
			mncwf_v4_event_descriptor_bytes);
		MncwfV4EventDescriptor event{};
		event.event_uuid = read_array<16>(record, 0);
		const auto taxonomy = read_u16(record, 16);
		event.event_type = read_u16(record, 18);
		const auto lifecycle = read_u16(record, 20);
		const auto time_quality = read_u16(record, 22);
		event.flags = read_u32(record, 24);
		event.phase_mask = read_u32(record, 28);
		const auto quantity = read_u16(record, 32);
		const auto unit = read_u16(record, 34);
		event.trigger_source = read_u16(record, 36);
		event.configuration_generation = read_u32(record, 40);
		event.severity = read_u32(record, 44);
		event.start_sequence = read_u64(record, 48);
		event.current_sequence = read_u64(record, 56);
		event.end_sequence = read_u64(record, 64);
		event.trigger_sequence = read_u64(record, 72);
		event.start_tai_nanoseconds = read_u64(record, 80);
		event.current_tai_nanoseconds = read_u64(record, 88);
		event.end_tai_nanoseconds = read_u64(record, 96);
		event.trigger_tai_nanoseconds = read_u64(record, 104);
		event.start_utc_nanoseconds = read_u64(record, 112);
		event.current_utc_nanoseconds = read_u64(record, 120);
		event.end_utc_nanoseconds = read_u64(record, 128);
		event.trigger_utc_nanoseconds = read_u64(record, 136);
		event.uncertainty_nanoseconds = read_u64(record, 144);
		event.reference_micro_units = read_s64(record, 152);
		event.threshold_micro_units = read_s64(record, 160);
		event.hysteresis_micro_units = read_s64(record, 168);
		for (std::size_t phase_index = 0; phase_index < 3; ++phase_index)
			event.extrema_micro_units[phase_index] =
				read_s64(record, 176u + phase_index * 8u);
		event.duration_samples = read_u64(record, 200);
		event.update_count = read_u64(record, 208);
		event.status = read_u32(record, 216);
		event.taxonomy_name = read_string(record, 224, event_section.blob,
			"event taxonomy name");
		event.label = read_string(record, 232, event_section.blob,
			"event label");
		event.settings_snapshot_json = read_string(record, 240,
			event_section.blob, "event settings snapshot");
		if (all_zero(event.event_uuid) ||
		    !event_ids.insert(event.event_uuid).second ||
		    !valid_event_taxonomy(taxonomy) || event.event_type == 0u ||
		    !valid_event_lifecycle(lifecycle) ||
		    !valid_time_quality(time_quality) ||
		    (event.flags & ~mncwf_event_known_flags) != 0u ||
		    (event.flags & mncwf_event_start_valid) == 0u ||
		    event.phase_mask == 0u ||
		    (event.phase_mask & ~mncwf_event_known_phase_mask) != 0u ||
		    !valid_quantity(quantity) ||
		    !valid_unit(unit) || read_u16(record, 38) != 0u ||
		    read_u32(record, 220) != 0u || read_u64(record, 248) != 0u ||
		    event.taxonomy_name.empty() || event.label.empty())
			reject("event descriptor is invalid");
		if ((event.flags & mncwf_event_current_valid) != 0u &&
		    event.current_sequence < event.start_sequence)
			reject("event current anchor precedes its start");
		if ((event.flags & mncwf_event_end_valid) != 0u &&
		    (event.end_sequence < event.start_sequence ||
		     ((event.flags & mncwf_event_current_valid) != 0u &&
		      event.end_sequence < event.current_sequence)))
			reject("event end anchor is out of order");
		const auto parsed_lifecycle =
			static_cast<MncwfEventLifecycle>(lifecycle);
		if ((parsed_lifecycle == MncwfEventLifecycle::update &&
		     (event.flags & mncwf_event_current_valid) == 0u) ||
		    ((parsed_lifecycle == MncwfEventLifecycle::end ||
		      parsed_lifecycle == MncwfEventLifecycle::abort ||
		      parsed_lifecycle == MncwfEventLifecycle::complete) &&
		     (event.flags & mncwf_event_end_valid) == 0u))
			reject("event lifecycle is missing its required anchor");
		const bool snapshot_valid =
			(event.flags & mncwf_event_settings_snapshot_valid) != 0u;
		if (snapshot_valid != !event.settings_snapshot_json.empty())
			reject("event settings-snapshot flag disagrees with its payload");
		event.taxonomy = static_cast<MncwfEventTaxonomy>(taxonomy);
		event.lifecycle = static_cast<MncwfEventLifecycle>(lifecycle);
		event.time_quality = static_cast<MncwfTimeQuality>(time_quality);
		event.quantity = static_cast<MncwfQuantity>(quantity);
		event.si_unit = static_cast<MncwfSiUnit>(unit);
		events_.push_back(std::move(event));
	}

	const auto quality_info =
		find_section(MncwfV4SectionType::quality_intervals);
	const auto quality_section = parse_section_envelope(quality_info,
		mncwf_v4_quality_interval_bytes, 0u,
		mncwf_v4_max_metadata_section_bytes /
			mncwf_v4_quality_interval_bytes,
		false);
	quality_intervals_.reserve(
		narrow_size(quality_info.item_count, "quality interval count"));
	for (std::uint64_t index = 0; index < quality_info.item_count; ++index) {
		const auto record = quality_section.records.subspan(
			narrow_size(index * mncwf_v4_quality_interval_bytes,
				"quality record"),
			mncwf_v4_quality_interval_bytes);
		MncwfV4QualityInterval quality{};
		quality.first_frame = read_u64(record, 0);
		quality.frame_count = read_u64(record, 8);
		quality.first_sequence = read_u64(record, 16);
		quality.last_sequence = read_u64(record, 24);
		quality.channel_mask = read_u64(record, 32);
		quality.flags = read_u32(record, 40);
		quality.severity = read_u16(record, 44);
		quality.source = read_u16(record, 46);
		quality.detail_code = read_u32(record, 48);
		if (quality.flags == 0u ||
		    (quality.flags & ~mncwf_quality_known_flags) != 0u ||
		    quality.last_sequence < quality.first_sequence ||
		    read_u32(record, 52) != 0u || read_u64(record, 56) != 0u)
			reject("quality interval is invalid");
		quality_intervals_.push_back(quality);
	}

	const auto lineage_info = find_section(MncwfV4SectionType::lineage);
	const auto lineage_section = parse_section_envelope(lineage_info,
		mncwf_v4_lineage_entry_bytes, 0u, mncwf_v4_max_lineage_entries,
		false);
	lineage_.reserve(narrow_size(lineage_info.item_count, "lineage count"));
	for (std::uint64_t index = 0; index < lineage_info.item_count; ++index) {
		const auto record = lineage_section.records.subspan(
			narrow_size(index * mncwf_v4_lineage_entry_bytes,
				"lineage record"),
			mncwf_v4_lineage_entry_bytes);
		MncwfV4LineageEntry lineage{};
		const auto relation = read_u16(record, 0);
		lineage.flags = read_u16(record, 2);
		lineage.related_capture_uuid = read_array<16>(record, 8);
		lineage.related_event_uuid = read_array<16>(record, 24);
		lineage.first_sequence = read_u64(record, 40);
		lineage.last_sequence = read_u64(record, 48);
		lineage.part_index = read_u32(record, 56);
		lineage.part_count = read_u32(record, 60);
		if (!valid_lineage_relation(relation) || lineage.flags != 0u ||
		    read_u32(record, 4) != 0u ||
		    all_zero(lineage.related_capture_uuid) ||
		    lineage.last_sequence < lineage.first_sequence ||
		    lineage.part_count == 0u ||
		    lineage.part_index >= lineage.part_count)
			reject("lineage entry is invalid");
		lineage.relation = static_cast<MncwfLineageRelation>(relation);
		lineage_.push_back(lineage);
	}

	const auto sample_info = find_section(MncwfV4SectionType::sample_data);
	if (sample_info.item_bytes == 0u ||
	    sample_info.item_bytes != expected_frame_bytes)
		reject("sample frame size disagrees with channel definitions");
	const auto sample_section = parse_section_envelope(sample_info,
		sample_info.item_bytes, 1u,
		mncwf_v4_max_file_bytes / sample_info.item_bytes, false);
	sample_data_ = sample_section.records;
	sample_frame_count_ = sample_info.item_count;
	sample_frame_bytes_ = sample_info.item_bytes;
	if (expected_first_frame != sample_frame_count_)
		reject("timebase segments do not cover every sample frame");

	const auto sequence_is_stored = [this](std::uint64_t sequence) {
		return std::ranges::any_of(timebase_segments_,
			[sequence](const auto &segment) {
				const auto last = segment.first_sequence +
					segment.source_frame_count - 1u;
				return sequence >= segment.first_sequence &&
				       sequence <= last;
			});
	};
	for (const auto &event : events_) {
		if (!sequence_is_stored(event.start_sequence) ||
		    ((event.flags & mncwf_event_current_valid) != 0u &&
		     !sequence_is_stored(event.current_sequence)) ||
		    ((event.flags & mncwf_event_end_valid) != 0u &&
		     !sequence_is_stored(event.end_sequence)) ||
		    ((event.flags & mncwf_event_trigger_valid) != 0u &&
		     !sequence_is_stored(event.trigger_sequence)))
			reject("event anchor lies outside the capture");
	}
	for (const auto &quality : quality_intervals_) {
		if (quality.first_frame > sample_frame_count_ ||
		    quality.frame_count >
			    sample_frame_count_ - quality.first_frame)
			reject("quality interval lies outside the sample data");
		if (channels_.size() < 64u &&
		    (quality.channel_mask >> channels_.size()) != 0u)
			reject("quality interval references an unknown channel");
	}
}

std::span<const std::byte> MncwfV4Reader::sample_frame(
	std::uint64_t index) const
{
	if (index >= sample_frame_count_)
		throw std::out_of_range("MNCWF v4 sample frame index");
	const auto offset = checked_multiply(index, sample_frame_bytes_,
		"sample frame offset");
	return sample_data_.subspan(narrow_size(offset, "sample frame offset"),
		sample_frame_bytes_);
}

MncwfV4ConversionReadiness
assess_mncwf_v4_conversion_readiness(const MncwfV4Reader &reader)
{
	MncwfV4ConversionReadiness result{};
	const auto both = [&result](std::string field) {
		result.comtrade_missing.push_back(field);
		result.pqdif_missing.push_back(std::move(field));
	};
	const auto &capture = reader.capture_metadata();
	if (capture.station_name.empty())
		both("capture.station_name");
	if (capture.site_name.empty())
		both("capture.site_name");
	if (capture.circuit_name.empty())
		both("capture.circuit_name");
	if (capture.device_model.empty())
		both("capture.device_model");
	if (capture.device_serial.empty())
		both("capture.device_serial");
	if (capture.calibration_id.empty())
		result.pqdif_missing.emplace_back("capture.calibration_id");
	if (capture.nominal_frequency_numerator == 0u)
		both("capture.nominal_frequency");
	if (capture.nominal_voltage_numerator <= 0)
		both("capture.nominal_voltage");
	if (capture.topology == MncwfTopology::unknown)
		result.pqdif_missing.emplace_back("capture.topology");

	for (std::size_t index = 0; index < reader.timebase_segments().size();
	     ++index) {
		const auto &segment = reader.timebase_segments()[index];
		const auto prefix = "timebase[" + std::to_string(index) + "].";
		if ((segment.flags & mncwf_time_utc_offset_known) == 0u)
			both(prefix + "utc_context");
		if (segment.correlation_tai_nanoseconds == 0u)
			both(prefix + "tai_correlation");
		if (segment.time_quality == MncwfTimeQuality::unknown)
			both(prefix + "time_quality");
		if (segment.clock_source == MncwfClockSource::unknown)
			result.pqdif_missing.push_back(prefix + "clock_source");
	}

	for (std::size_t index = 0; index < reader.channels().size(); ++index) {
		const auto &channel = reader.channels()[index];
		const auto prefix = "channel[" + std::to_string(index) + "].";
		if (channel.phase == MncwfPhase::none)
			both(prefix + "phase");
		if (channel.quantity == MncwfQuantity::unknown)
			both(prefix + "quantity");
		const bool unit_matches =
			(channel.quantity == MncwfQuantity::current &&
			 channel.si_unit == MncwfSiUnit::ampere) ||
			(channel.quantity == MncwfQuantity::voltage &&
			 channel.si_unit == MncwfSiUnit::volt) ||
			(channel.quantity == MncwfQuantity::frequency &&
			 channel.si_unit == MncwfSiUnit::hertz) ||
			((channel.quantity == MncwfQuantity::status ||
			  channel.quantity == MncwfQuantity::ratio) &&
			 channel.si_unit == MncwfSiUnit::dimensionless);
		if (!unit_matches)
			both(prefix + "quantity_unit_mapping");
		if ((channel.flags & mncwf_channel_transform_valid) == 0u)
			both(prefix + "affine_transform");
		if ((channel.flags & mncwf_channel_ratio_valid) == 0u)
			both(prefix + "primary_secondary_ratio");
		if ((channel.flags & mncwf_channel_nominal_valid) == 0u)
			result.pqdif_missing.push_back(prefix + "nominal");
		if ((channel.flags & mncwf_channel_range_valid) == 0u)
			both(prefix + "range");
		if ((channel.flags & mncwf_channel_resolution_valid) == 0u)
			result.pqdif_missing.push_back(prefix + "resolution");
		if ((channel.flags & mncwf_channel_clipping_valid) == 0u)
			both(prefix + "clipping");
		if ((channel.flags & mncwf_channel_calibration_valid) == 0u)
			result.pqdif_missing.push_back(prefix + "calibration");
	}

	if (reader.events().empty()) {
		both("event_descriptor");
	} else {
		for (std::size_t index = 0; index < reader.events().size(); ++index) {
			const auto &event = reader.events()[index];
			const auto prefix = "event[" + std::to_string(index) + "].";
			if (event.taxonomy == MncwfEventTaxonomy::unknown)
				both(prefix + "taxonomy");
			if ((event.flags & mncwf_event_trigger_valid) == 0u)
				result.comtrade_missing.push_back(prefix + "trigger_sequence");
			if ((event.flags & mncwf_event_utc_valid) == 0u)
				both(prefix + "utc_anchors");
			if ((event.flags & mncwf_event_tai_valid) == 0u)
				both(prefix + "tai_anchors");
			if ((event.flags & mncwf_event_settings_snapshot_valid) == 0u)
				both(prefix + "settings_snapshot");
		}
	}

	return result;
}

} // namespace msap1
