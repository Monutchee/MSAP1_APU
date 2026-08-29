#include "msap1/waveform/mncwf_v4.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <numeric>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace msap1 {
namespace {

[[noreturn]] void reject(std::string_view reason)
{
	throw std::invalid_argument("MNCWF v4: " + std::string(reason));
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
	constexpr std::uint32_t polynomial = 0x82F63B78u;
	std::uint32_t crc = 0xffffffffu;
	for (const auto value : bytes) {
		crc ^= std::to_integer<std::uint8_t>(value);
		for (unsigned bit = 0; bit < 8u; ++bit)
			crc = (crc >> 1u) ^ ((crc & 1u) != 0u ? polynomial : 0u);
	}
	return crc ^ 0xffffffffu;
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
