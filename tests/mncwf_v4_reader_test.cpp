#include "msap1/waveform/mncwf_v4.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using Bytes = std::vector<std::byte>;

int failures = 0;

void require(bool condition, std::string_view what)
{
	if (!condition) {
		std::fprintf(stderr, "FAIL: %.*s\n", static_cast<int>(what.size()),
			what.data());
		++failures;
	}
}

void require_space(const Bytes &bytes, std::size_t offset, std::size_t count)
{
	if (offset > bytes.size() || count > bytes.size() - offset)
		throw std::logic_error("test fixture write is out of bounds");
}

void put_u16(Bytes &bytes, std::size_t offset, std::uint16_t value)
{
	require_space(bytes, offset, 2);
	for (unsigned index = 0; index < 2; ++index)
		bytes[offset + index] =
			std::byte{static_cast<std::uint8_t>(value >> (index * 8u))};
}

void put_u32(Bytes &bytes, std::size_t offset, std::uint32_t value)
{
	require_space(bytes, offset, 4);
	for (unsigned index = 0; index < 4; ++index)
		bytes[offset + index] =
			std::byte{static_cast<std::uint8_t>(value >> (index * 8u))};
}

void put_s32(Bytes &bytes, std::size_t offset, std::int32_t value)
{
	put_u32(bytes, offset, std::bit_cast<std::uint32_t>(value));
}

void put_u64(Bytes &bytes, std::size_t offset, std::uint64_t value)
{
	require_space(bytes, offset, 8);
	for (unsigned index = 0; index < 8; ++index)
		bytes[offset + index] =
			std::byte{static_cast<std::uint8_t>(value >> (index * 8u))};
}

void put_s64(Bytes &bytes, std::size_t offset, std::int64_t value)
{
	put_u64(bytes, offset, std::bit_cast<std::uint64_t>(value));
}

std::uint32_t get_u32(std::span<const std::byte> bytes, std::size_t offset)
{
	if (offset > bytes.size() || bytes.size() - offset < 4)
		throw std::logic_error("test fixture read is out of bounds");
	std::uint32_t result = 0;
	for (unsigned index = 0; index < 4; ++index)
		result |= static_cast<std::uint32_t>(
			std::to_integer<std::uint8_t>(bytes[offset + index]))
			<< (index * 8u);
	return result;
}

std::uint64_t get_u64(std::span<const std::byte> bytes, std::size_t offset)
{
	if (offset > bytes.size() || bytes.size() - offset < 8)
		throw std::logic_error("test fixture read is out of bounds");
	std::uint64_t result = 0;
	for (unsigned index = 0; index < 8; ++index)
		result |= static_cast<std::uint64_t>(
			std::to_integer<std::uint8_t>(bytes[offset + index]))
			<< (index * 8u);
	return result;
}

void fill_identity(Bytes &record, std::size_t offset, std::size_t count,
	std::uint8_t seed)
{
	require_space(record, offset, count);
	for (std::size_t index = 0; index < count; ++index)
		record[offset + index] =
			std::byte{static_cast<std::uint8_t>(seed + index)};
}

void align_eight(Bytes &bytes)
{
	while ((bytes.size() & 7u) != 0u)
		bytes.push_back(std::byte{0});
}

std::pair<std::uint32_t, std::uint32_t> append_string(Bytes &blob,
	std::string_view text)
{
	const auto offset = static_cast<std::uint32_t>(blob.size());
	for (const char value : text)
		blob.push_back(std::byte{static_cast<std::uint8_t>(value)});
	return {offset, static_cast<std::uint32_t>(text.size())};
}

void put_string_ref(Bytes &record, std::size_t offset,
	std::pair<std::uint32_t, std::uint32_t> ref)
{
	put_u32(record, offset, ref.first);
	put_u32(record, offset + 4u, ref.second);
}

struct Section {
	std::uint32_t type = 0;
	std::uint16_t version = 1;
	std::uint16_t flags = msap1::mncwf_v4_section_required;
	std::uint64_t item_count = 0;
	std::uint32_t item_bytes = 0;
	Bytes data;
};

Section enveloped_section(msap1::MncwfV4SectionType type,
	std::uint64_t item_count, std::uint32_t item_bytes, Bytes records,
	Bytes blob = {})
{
	if (records.size() != item_count * item_bytes)
		throw std::logic_error("test section record geometry is inconsistent");
	Section section{};
	section.type = static_cast<std::uint32_t>(type);
	section.item_count = item_count;
	section.item_bytes = item_bytes;
	section.data.resize(msap1::mncwf_v4_section_header_bytes);
	section.data.insert(section.data.end(), records.begin(), records.end());
	std::uint64_t blob_offset = 0;
	if (!blob.empty()) {
		align_eight(section.data);
		blob_offset = section.data.size();
		section.data.insert(section.data.end(), blob.begin(), blob.end());
	}
	put_u32(section.data, 0, section.type);
	put_u16(section.data, 4, section.version);
	put_u16(section.data, 6, msap1::mncwf_v4_section_header_bytes);
	put_u32(section.data, 12, item_bytes);
	put_u64(section.data, 16, item_count);
	put_u64(section.data, 24, blob_offset);
	put_u64(section.data, 32, blob.size());
	return section;
}

Section capture_section()
{
	Bytes record(msap1::mncwf_v4_capture_metadata_bytes);
	Bytes blob;
	fill_identity(record, 0, 16, 0x10);
	fill_identity(record, 16, 16, 0x30);
	fill_identity(record, 32, 32, 0x50);
	fill_identity(record, 64, 32, 0x80);
	put_u64(record, 96, 2'000'000'000ull);
	put_u64(record, 104, 1'999'999'963ull);
	put_s64(record, 112, 230);
	put_u64(record, 120, 1);
	put_u64(record, 128, 60);
	put_u64(record, 136, 1);
	put_u32(record, 144, static_cast<std::uint32_t>(msap1::MncwfTopology::wye));
	put_u32(record, 148, static_cast<std::uint32_t>(
		msap1::MncwfCalibrationStatus::valid));
	const std::array<std::string_view, 12> strings{
		"Substation A", "Toronto lab", "Feeder 7", "MSAP1",
		"MSAP1 neutral sensor board", "18.0.0", "build-20260829",
		"msap1-default-32ksps", "settings-generation-7", "cal-2026-01",
		"SN0007", "synthetic conversion-readiness fixture"};
	for (std::size_t index = 0; index < strings.size(); ++index)
		put_string_ref(record, 160u + index * 8u,
			append_string(blob, strings[index]));
	return enveloped_section(msap1::MncwfV4SectionType::capture_metadata,
		1, msap1::mncwf_v4_capture_metadata_bytes, std::move(record),
		std::move(blob));
}

Section timebase_section()
{
	Bytes record(msap1::mncwf_v4_timebase_segment_bytes);
	put_u64(record, 0, 0);              // first persisted frame
	put_u64(record, 8, 3);              // persisted frames
	put_u64(record, 16, 100);           // first acquisition sequence
	put_u64(record, 24, 4);             // acquisition sequence step
	put_u64(record, 32, 32'000);        // acquisition rate numerator
	put_u64(record, 40, 1);
	put_u64(record, 48, 8'000);         // persisted rate numerator
	put_u64(record, 56, 1);
	put_u64(record, 64, 100);           // correlation sequence
	put_u64(record, 72, 0x1234);        // correlation PL tick
	put_u64(record, 80, 2'000'000'000ull);
	put_u64(record, 88, 1'999'999'963ull);
	put_u64(record, 96, 250);
	put_u32(record, 104, 4);
	put_u16(record, 108, static_cast<std::uint16_t>(
		msap1::MncwfDecimationMethod::boxcar_mean_toward_zero));
	put_u16(record, 110,
		static_cast<std::uint16_t>(msap1::MncwfClockSource::ptp));
	put_u16(record, 112,
		static_cast<std::uint16_t>(msap1::MncwfTimeQuality::locked));
	put_u16(record, 114, msap1::mncwf_time_utc_offset_known);
	put_s32(record, 116, -4 * 60 * 60);
	put_u64(record, 120, 10);           // final group represents two frames
	return enveloped_section(msap1::MncwfV4SectionType::timebase_segments,
		1, msap1::mncwf_v4_timebase_segment_bytes, std::move(record));
}

void append_channel(Bytes &records, Bytes &blob, std::uint8_t id_seed,
	std::uint32_t source, msap1::MncwfQuantity quantity,
	msap1::MncwfSiUnit unit, std::string_view name, std::string_view symbol)
{
	Bytes record(msap1::mncwf_v4_channel_definition_bytes);
	fill_identity(record, 0, 16, id_seed);
	put_u32(record, 16, source);
	put_u32(record, 20, msap1::mncwf_channel_enabled |
		msap1::mncwf_channel_transform_valid |
		msap1::mncwf_channel_ratio_valid |
		msap1::mncwf_channel_nominal_valid |
		msap1::mncwf_channel_range_valid |
		msap1::mncwf_channel_resolution_valid |
		msap1::mncwf_channel_clipping_valid |
		msap1::mncwf_channel_calibration_valid);
	put_u16(record, 24, static_cast<std::uint16_t>(msap1::MncwfPhase::a));
	put_u16(record, 26, static_cast<std::uint16_t>(quantity));
	put_u16(record, 28, static_cast<std::uint16_t>(unit));
	put_u16(record, 30, static_cast<std::uint16_t>(
		msap1::MncwfSampleEncoding::signed_integer_little_endian));
	put_u16(record, 32, 32);
	put_u16(record, 34, 24);
	put_s64(record, 40, 1);
	put_u64(record, 48, 1'000'000);
	put_s64(record, 56, 0);
	put_u64(record, 64, 1);
	put_u64(record, 72, 1);
	put_u64(record, 80, 1);
	put_s64(record, 88,
		quantity == msap1::MncwfQuantity::voltage ? 230 : 5);
	put_u64(record, 96, 1);
	put_s64(record, 104, -10'000);
	put_u64(record, 112, 1);
	put_s64(record, 120, 10'000);
	put_u64(record, 128, 1);
	put_u64(record, 136, 1);
	put_u64(record, 144, 1'000'000);
	put_s64(record, 152, -8'388'608);
	put_s64(record, 160, 8'388'607);
	put_string_ref(record, 168, append_string(blob, name));
	put_string_ref(record, 176, append_string(blob, symbol));
	put_string_ref(record, 184,
		append_string(blob, "phase-A acquisition channel"));
	records.insert(records.end(), record.begin(), record.end());
}

Section channel_section()
{
	Bytes records;
	Bytes blob;
	append_channel(records, blob, 0xa0, 0, msap1::MncwfQuantity::current,
		msap1::MncwfSiUnit::ampere, "Ia", "A");
	append_channel(records, blob, 0xc0, 6, msap1::MncwfQuantity::voltage,
		msap1::MncwfSiUnit::volt, "Va", "V");
	return enveloped_section(msap1::MncwfV4SectionType::channel_definitions,
		2, msap1::mncwf_v4_channel_definition_bytes, std::move(records),
		std::move(blob));
}

Section event_section()
{
	Bytes record(msap1::mncwf_v4_event_descriptor_bytes);
	Bytes blob;
	fill_identity(record, 0, 16, 0xe0);
	put_u16(record, 16,
		static_cast<std::uint16_t>(msap1::MncwfEventTaxonomy::iec_61000_4_30));
	put_u16(record, 18, 1); // voltage dip within the named taxonomy
	put_u16(record, 20,
		static_cast<std::uint16_t>(msap1::MncwfEventLifecycle::complete));
	put_u16(record, 22,
		static_cast<std::uint16_t>(msap1::MncwfTimeQuality::locked));
	put_u32(record, 24, msap1::mncwf_event_start_valid |
		msap1::mncwf_event_current_valid | msap1::mncwf_event_end_valid |
		msap1::mncwf_event_trigger_valid | msap1::mncwf_event_tai_valid |
		msap1::mncwf_event_utc_valid |
		msap1::mncwf_event_settings_snapshot_valid);
	put_u32(record, 28, 0x1); // phase A
	put_u16(record, 32,
		static_cast<std::uint16_t>(msap1::MncwfQuantity::voltage));
	put_u16(record, 34,
		static_cast<std::uint16_t>(msap1::MncwfSiUnit::volt));
	put_u16(record, 36, 3); // PQ event trigger
	put_u32(record, 40, 7); // capture-time configuration generation
	put_u32(record, 44, 2);
	put_u64(record, 48, 100);
	put_u64(record, 56, 105);
	put_u64(record, 64, 109);
	put_u64(record, 72, 104);
	put_u64(record, 80, 2'000'000'000ull);
	put_u64(record, 88, 2'000'156'250ull);
	put_u64(record, 96, 2'000'281'250ull);
	put_u64(record, 104, 2'000'125'000ull);
	put_u64(record, 112, 1'999'999'963ull);
	put_u64(record, 120, 2'000'156'213ull);
	put_u64(record, 128, 2'000'281'213ull);
	put_u64(record, 136, 2'000'124'963ull);
	put_u64(record, 144, 250);
	put_s64(record, 152, 230'000'000);
	put_s64(record, 160, 207'000'000);
	put_s64(record, 168, 2'300'000);
	put_s64(record, 176, 190'000'000);
	put_s64(record, 184, 230'000'000);
	put_s64(record, 192, 230'000'000);
	put_u64(record, 200, 10);
	put_u64(record, 208, 2);
	put_string_ref(record, 224,
		append_string(blob, "IEC 61000-4-30 voltage event"));
	put_string_ref(record, 232, append_string(blob, "phase-A voltage dip"));
	put_string_ref(record, 240, append_string(blob,
		R"({"generation":7,"threshold_e4":9000,"hysteresis_e4":100})"));
	return enveloped_section(msap1::MncwfV4SectionType::event_descriptors,
		1, msap1::mncwf_v4_event_descriptor_bytes, std::move(record),
		std::move(blob));
}

Section quality_section()
{
	Bytes record(msap1::mncwf_v4_quality_interval_bytes);
	put_u64(record, 0, 2);
	put_u64(record, 8, 1);
	put_u64(record, 16, 108);
	put_u64(record, 24, 109);
	put_u64(record, 32, 0x2); // Va only
	put_u32(record, 40, msap1::mncwf_quality_transport_loss);
	put_u16(record, 44, 2);
	put_u16(record, 46, 1);
	put_u32(record, 48, 7);
	return enveloped_section(msap1::MncwfV4SectionType::quality_intervals,
		1, msap1::mncwf_v4_quality_interval_bytes, std::move(record));
}

Section lineage_section()
{
	Bytes record(msap1::mncwf_v4_lineage_entry_bytes);
	put_u16(record, 0,
		static_cast<std::uint16_t>(msap1::MncwfLineageRelation::parent));
	fill_identity(record, 8, 16, 0x21);
	fill_identity(record, 24, 16, 0xe0);
	put_u64(record, 40, 100);
	put_u64(record, 48, 109);
	put_u32(record, 56, 0);
	put_u32(record, 60, 1);
	return enveloped_section(msap1::MncwfV4SectionType::lineage, 1,
		msap1::mncwf_v4_lineage_entry_bytes, std::move(record));
}

Section sample_section()
{
	Bytes records(3 * 2 * sizeof(std::int32_t));
	for (std::size_t frame = 0; frame < 3; ++frame) {
		put_s32(records, frame * 8, static_cast<std::int32_t>(10 + frame));
		put_s32(records, frame * 8 + 4,
			static_cast<std::int32_t>(230'000 + frame));
	}
	return enveloped_section(msap1::MncwfV4SectionType::sample_data, 3, 8,
		std::move(records));
}

void refresh_checksums(Bytes &file)
{
	put_u32(file, 56, 0);
	const auto count = get_u32(file, 20);
	const auto directory_bytes =
		static_cast<std::size_t>(count) * msap1::mncwf_v4_directory_entry_bytes;
	put_u32(file, 52, msap1::mncwf_crc32c(std::span<const std::byte>{file}.subspan(
		msap1::mncwf_v4_header_bytes, directory_bytes)));
	put_u32(file, 56, msap1::mncwf_crc32c(
		std::span<const std::byte>{file}.first(msap1::mncwf_v4_header_bytes)));
}

Bytes make_valid_file()
{
	std::vector<Section> sections;
	sections.push_back(capture_section());
	sections.push_back(timebase_section());
	sections.push_back(channel_section());
	sections.push_back(event_section());
	sections.push_back(quality_section());
	sections.push_back(lineage_section());
	sections.push_back(sample_section());
	sections.push_back(Section{0x8000'0001u, 1, 0, 0, 0,
		Bytes{std::byte{'M'}, std::byte{'1'}, std::byte{'8'}, std::byte{0},
			std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}}});

	const auto directory_bytes =
		sections.size() * msap1::mncwf_v4_directory_entry_bytes;
	Bytes file(msap1::mncwf_v4_header_bytes + directory_bytes);
	std::copy(msap1::mncwf_magic.begin(), msap1::mncwf_magic.end(), file.begin());
	put_u32(file, 8, msap1::mncwf_v4_version);
	put_u32(file, 12, msap1::mncwf_v4_header_bytes);
	put_u32(file, 16, msap1::mncwf_v4_directory_entry_bytes);
	put_u32(file, 20, static_cast<std::uint32_t>(sections.size()));
	put_u64(file, 24, msap1::mncwf_v4_header_bytes);
	put_u64(file, 32, directory_bytes);

	for (std::size_t index = 0; index < sections.size(); ++index) {
		align_eight(file);
		const auto section_offset = file.size();
		const auto &section = sections[index];
		file.insert(file.end(), section.data.begin(), section.data.end());
		const auto entry = msap1::mncwf_v4_header_bytes +
			index * msap1::mncwf_v4_directory_entry_bytes;
		put_u32(file, entry, section.type);
		put_u16(file, entry + 4, section.version);
		put_u16(file, entry + 6, section.flags);
		put_u64(file, entry + 8, section_offset);
		put_u64(file, entry + 16, section.data.size());
		put_u64(file, entry + 24, section.data.size());
		put_u64(file, entry + 32, section.item_count);
		put_u32(file, entry + 40, section.item_bytes);
		put_u32(file, entry + 44, msap1::mncwf_crc32c(section.data));
	}
	align_eight(file);
	put_u64(file, 40, file.size());
	refresh_checksums(file);
	return file;
}

std::size_t entry_for(const Bytes &file, std::uint32_t type)
{
	const auto count = get_u32(file, 20);
	for (std::uint32_t index = 0; index < count; ++index) {
		const auto entry = msap1::mncwf_v4_header_bytes +
			index * msap1::mncwf_v4_directory_entry_bytes;
		if (get_u32(file, entry) == type)
			return entry;
	}
	throw std::logic_error("test fixture section was not found");
}

void refresh_section_crc(Bytes &file, std::size_t entry)
{
	const auto offset = static_cast<std::size_t>(get_u64(file, entry + 8));
	const auto bytes = static_cast<std::size_t>(get_u64(file, entry + 16));
	put_u32(file, entry + 44, msap1::mncwf_crc32c(
		std::span<const std::byte>{file}.subspan(offset, bytes)));
	refresh_checksums(file);
}

void expect_rejected(const Bytes &file, std::string_view what)
{
	try {
		(void)msap1::MncwfV4Reader{file};
		require(false, what);
	} catch (const std::invalid_argument &) {
		// Expected: readers reject before exposing any sample view.
	}
}

} // namespace

int main()
{
	static constexpr std::array<char, 9> crc_vector{
		'1', '2', '3', '4', '5', '6', '7', '8', '9'};
	require(msap1::mncwf_crc32c(std::as_bytes(std::span{crc_vector})) ==
			0xe3069283u,
		"CRC32C matches the Castagnoli check vector");

	const auto valid = make_valid_file();
	try {
		const msap1::MncwfV4Reader reader{valid};
		const auto readiness =
			msap1::assess_mncwf_v4_conversion_readiness(reader);
		require(readiness.comtrade_ready() && readiness.pqdif_ready(),
			"fixture contains every future-converter source field");
		require(reader.header().section_count == 8,
			"optional section remains visible in the directory view");
		require(reader.capture_metadata().station_name == "Substation A" &&
			reader.capture_metadata().configuration_id ==
				"settings-generation-7",
			"capture-time identity snapshot");
		require(reader.timebase_segments().size() == 1 &&
			reader.timebase_segments().front().decimation_divisor == 4 &&
			reader.timebase_segments().front().source_frame_count == 10,
			"exact decimated short-final-group geometry");
		require(reader.channels().size() == 2 &&
			reader.channels()[1].name == "Va" &&
			reader.channels()[1].source_channel == 6,
			"channel identity and source mapping");
		require(reader.events().size() == 1 &&
			reader.events().front().settings_snapshot_json.find(
				"threshold_e4") != std::string::npos,
			"historical event includes its evaluated settings");
		require(reader.quality_intervals().size() == 1 &&
			reader.lineage().size() == 1,
			"quality and lineage metadata");
		require(reader.sample_frame_count() == 3 &&
			reader.sample_frame_bytes() == 8 &&
			std::bit_cast<std::int32_t>(get_u32(reader.sample_frame(2), 4)) ==
				230'002,
			"sample geometry and random access");
		bool bounds_checked = false;
		try {
			(void)reader.sample_frame(3);
		} catch (const std::out_of_range &) {
			bounds_checked = true;
		}
		require(bounds_checked, "sample-frame access is bounds checked");

		msap1::MncwfV4Document document{};
		document.capture_metadata = reader.capture_metadata();
		document.timebase_segments = reader.timebase_segments();
		document.channels = reader.channels();
		document.events = reader.events();
		document.quality_intervals = reader.quality_intervals();
		document.lineage = reader.lineage();
		document.sample_frame_count = reader.sample_frame_count();
		document.sample_frame_bytes = reader.sample_frame_bytes();
		document.sample_data.assign(reader.sample_data().begin(),
			reader.sample_data().end());
		const auto encoded = msap1::encode_mncwf_v4(document);
		const msap1::MncwfV4Reader round_trip{encoded};
		require(round_trip.header().section_count == 7u &&
			round_trip.capture_metadata().configuration_sha256 ==
				reader.capture_metadata().configuration_sha256 &&
			round_trip.timebase_segments().size() ==
				reader.timebase_segments().size() &&
			round_trip.timebase_segments().front().source_frame_count ==
				reader.timebase_segments().front().source_frame_count &&
			round_trip.sample_data().size() == reader.sample_data().size(),
			"typed writer round-trips every mandatory section");
		require(msap1::encode_mncwf_v4(document) == encoded,
			"typed writer output is deterministic");
	} catch (const std::exception &error) {
		std::fprintf(stderr, "FAIL: valid fixture rejected: %s\n", error.what());
		++failures;
	}
	{
		const auto uuid = msap1::mncwf_random_uuid();
		require(std::ranges::any_of(uuid,
			[](std::byte byte) { return byte != std::byte{0}; }) &&
			(std::to_integer<std::uint8_t>(uuid[6]) & 0xf0u) == 0x40u &&
			(std::to_integer<std::uint8_t>(uuid[8]) & 0xc0u) == 0x80u,
			"capture UUID uses the RFC-4122 random layout");
		const auto digest = msap1::mncwf_sha256("abc");
		require(std::to_integer<std::uint8_t>(digest[0]) == 0xbau &&
			std::to_integer<std::uint8_t>(digest[31]) == 0xadu,
			"capture digest helper computes SHA-256");
	}
	{
		bool all_rejected = true;
		for (std::size_t index = 0; index < valid.size(); ++index) {
			auto corrupted = valid;
			corrupted[index] ^= std::byte{1};
			try {
				(void)msap1::MncwfV4Reader{corrupted};
				all_rejected = false;
				std::fprintf(stderr,
					"FAIL: single-bit corruption accepted at byte %zu\n",
					index);
				break;
			} catch (const std::invalid_argument &) {
			}
		}
		require(all_rejected,
			"every file byte is protected by CRC or zero-padding validation");
	}

	{
		auto bad = valid;
		bad[56] ^= std::byte{1};
		expect_rejected(bad, "header CRC corruption is rejected");
	}
	{
		auto bad = valid;
		bad[msap1::mncwf_v4_header_bytes + 48] ^= std::byte{1};
		expect_rejected(bad, "directory CRC corruption is rejected");
	}
	{
		auto bad = valid;
		const auto entry = entry_for(bad,
			static_cast<std::uint32_t>(msap1::MncwfV4SectionType::sample_data));
		bad[static_cast<std::size_t>(get_u64(bad, entry + 8)) +
			msap1::mncwf_v4_section_header_bytes] ^= std::byte{1};
		expect_rejected(bad, "section CRC corruption is rejected");
	}
	{
		auto bad = valid;
		bad.pop_back();
		expect_rejected(bad, "truncated file is rejected");
	}
	{
		auto bad = valid;
		const auto unknown = entry_for(bad, 0x8000'0001u);
		put_u16(bad, unknown + 6, msap1::mncwf_v4_section_required);
		refresh_checksums(bad);
		expect_rejected(bad, "unknown required section is rejected");
	}
	{
		auto bad = valid;
		const auto unknown = entry_for(bad, 0x8000'0001u);
		put_u32(bad, unknown, static_cast<std::uint32_t>(
			msap1::MncwfV4SectionType::capture_metadata));
		put_u16(bad, unknown + 6, msap1::mncwf_v4_section_required);
		refresh_checksums(bad);
		expect_rejected(bad, "duplicate mandatory section is rejected");
	}
	{
		auto bad = valid;
		const auto unknown = entry_for(bad, 0x8000'0001u);
		const auto samples = entry_for(bad,
			static_cast<std::uint32_t>(msap1::MncwfV4SectionType::sample_data));
		put_u64(bad, unknown + 8, get_u64(bad, samples + 8));
		refresh_checksums(bad);
		expect_rejected(bad, "overlapping sections are rejected");
	}
	{
		auto bad = valid;
		const auto unknown = entry_for(bad, 0x8000'0001u);
		put_u64(bad, unknown + 8, bad.size() + 8u);
		refresh_checksums(bad);
		expect_rejected(bad, "out-of-range section is rejected");
	}
	{
		auto bad = valid;
		const auto time = entry_for(bad, static_cast<std::uint32_t>(
			msap1::MncwfV4SectionType::timebase_segments));
		const auto record = static_cast<std::size_t>(get_u64(bad, time + 8)) +
			msap1::mncwf_v4_section_header_bytes;
		put_u64(bad, record + 120, 8); // minimum is 9 for three groups of four
		refresh_section_crc(bad, time);
		expect_rejected(bad, "inconsistent decimation geometry is rejected");
	}
	{
		auto bad = valid;
		const auto capture = entry_for(bad, static_cast<std::uint32_t>(
			msap1::MncwfV4SectionType::capture_metadata));
		const auto section = static_cast<std::size_t>(get_u64(bad, capture + 8));
		const auto blob = static_cast<std::size_t>(get_u64(bad, section + 24));
		bad[section + blob] = std::byte{0xc0}; // overlong/invalid UTF-8 lead byte
		refresh_section_crc(bad, capture);
		expect_rejected(bad, "invalid UTF-8 metadata is rejected");
	}
	{
		auto incomplete = valid;
		const auto capture = entry_for(incomplete, static_cast<std::uint32_t>(
			msap1::MncwfV4SectionType::capture_metadata));
		const auto record = static_cast<std::size_t>(
			get_u64(incomplete, capture + 8)) +
			msap1::mncwf_v4_section_header_bytes;
		put_u32(incomplete, record + 160, 0);
		put_u32(incomplete, record + 164, 0);
		refresh_section_crc(incomplete, capture);
		const msap1::MncwfV4Reader reader{incomplete};
		const auto readiness =
			msap1::assess_mncwf_v4_conversion_readiness(reader);
		require(!readiness.comtrade_ready() && !readiness.pqdif_ready() &&
			std::ranges::find(readiness.comtrade_missing,
				"capture.station_name") !=
				readiness.comtrade_missing.end(),
			"readiness reports missing capture-time identity");
	}

	if (failures != 0)
		std::fprintf(stderr, "%d MNCWF v4 reader test(s) failed\n", failures);
	return failures == 0 ? 0 : 1;
}
