#include "mnc/waveform/comtrade_converter.hpp"
#include "mnc/waveform/comtrade_zip_converter.hpp"
#include "mnc/waveform/pqdif_converter.hpp"

#include <openssl/evp.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace mnc::waveform;

void require(bool condition, std::string_view message)
{
	if (!condition)
		throw std::runtime_error(std::string(message));
}

Uuid uuid(std::uint8_t seed)
{
	Uuid value{};
	for (std::size_t index = 0; index != value.size(); ++index)
		value[index] = static_cast<std::byte>(seed + index);
	return value;
}

std::uint16_t u16(std::span<const std::byte> bytes, std::size_t offset)
{
	require(offset + 2u <= bytes.size(), "u16 read exceeds buffer");
	return std::to_integer<std::uint16_t>(bytes[offset]) |
		(std::to_integer<std::uint16_t>(bytes[offset + 1u]) << 8u);
}

std::uint32_t u32(std::span<const std::byte> bytes, std::size_t offset)
{
	require(offset + 4u <= bytes.size(), "u32 read exceeds buffer");
	std::uint32_t value = 0;
	for (unsigned index = 0; index != 4; ++index)
		value |= std::to_integer<std::uint32_t>(bytes[offset + index]) <<
			(index * 8u);
	return value;
}

std::string text(std::span<const std::byte> bytes)
{
	return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}

std::string sha256(std::span<const std::byte> bytes)
{
	EVP_MD_CTX *context = EVP_MD_CTX_new();
	require(context != nullptr, "allocate SHA-256 context");
	std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
	unsigned int size = 0;
	const bool ok = EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1 &&
		EVP_DigestUpdate(context, bytes.data(), bytes.size()) == 1 &&
		EVP_DigestFinal_ex(context, digest.data(), &size) == 1;
	EVP_MD_CTX_free(context);
	require(ok && size == 32u, "calculate SHA-256");
	constexpr char hex[] = "0123456789abcdef";
	std::string result;
	result.reserve(size * 2u);
	for (unsigned index = 0; index != size; ++index) {
		result.push_back(hex[digest[index] >> 4u]);
		result.push_back(hex[digest[index] & 0x0fu]);
	}
	return result;
}

class MemorySource final : public WaveformSource {
public:
	MemorySource()
	{
		metadata_.capture.capture_uuid = uuid(0x10);
		metadata_.capture.source_capture_uuid = uuid(0x20);
		metadata_.capture.device_uuid = uuid(0x30);
		metadata_.capture.configuration_sha256.fill(std::byte{0x44});
		metadata_.capture.sensor_profile_sha256.fill(std::byte{0x55});
		metadata_.capture.created_tai_nanoseconds = 1'700'000'037'000'000'000ull;
		metadata_.capture.created_utc_nanoseconds = 1'700'000'000'000'000'000ull;
		metadata_.capture.nominal_voltage = {120, 1};
		metadata_.capture.nominal_frequency = {60, 1};
		metadata_.capture.topology = Topology::wye;
		metadata_.capture.calibration_status = CalibrationStatus::valid;
		metadata_.capture.station_name = "Station One";
		metadata_.capture.site_name = "Lab";
		metadata_.capture.circuit_name = "Feeder A";
		metadata_.capture.product_name = "MNC meter";
		metadata_.capture.device_model = "MSAP1";
		metadata_.capture.firmware_version = "1.2.3";
		metadata_.capture.software_build_id = "test-build";
		metadata_.capture.sensor_profile_id = "sensor-profile";
		metadata_.capture.configuration_id = "configuration";
		metadata_.capture.calibration_id = "calibration";
		metadata_.capture.device_serial = "serial-42";
		metadata_.capture.comments = "deterministic converter vector";

		metadata_.timebase_segments = {
			{.first_frame = 0, .frame_count = 2, .first_sequence = 100,
			 .sequence_step = 1, .acquisition_rate = {1000, 1},
			 .persisted_rate = {1000, 1}, .correlation_sequence = 100,
			 .correlation_pl_tick = 500, .correlation_tai_nanoseconds =
				1'700'000'037'000'000'000ull,
			 .correlation_utc_nanoseconds = 1'700'000'000'000'000'000ull,
			 .uncertainty_nanoseconds = 40, .decimation_divisor = 1,
			 .decimation_method = DecimationMethod::none,
			 .clock_source = ClockSource::ptp,
			 .time_quality = TimeQuality::locked,
			 .flags = time_utc_offset_known, .utc_offset_seconds = -18'000,
			 .source_frame_count = 2},
			{.first_frame = 2, .frame_count = 2, .first_sequence = 102,
			 .sequence_step = 1, .acquisition_rate = {2000, 1},
			 .persisted_rate = {2000, 1}, .correlation_sequence = 102,
			 .correlation_pl_tick = 502, .correlation_tai_nanoseconds =
				1'700'000'037'002'000'000ull,
			 .correlation_utc_nanoseconds = 1'700'000'000'002'000'000ull,
			 .uncertainty_nanoseconds = 45, .decimation_divisor = 1,
			 .decimation_method = DecimationMethod::none,
			 .clock_source = ClockSource::ptp,
			 .time_quality = TimeQuality::locked,
			 .flags = static_cast<std::uint16_t>(time_utc_offset_known |
				time_rate_change_before), .utc_offset_seconds = -18'000,
			 .source_frame_count = 2},
		};

		ChannelDefinition voltage;
		voltage.stable_id = uuid(0x40);
		voltage.source_channel = 0;
		voltage.flags = channel_enabled | channel_transform_valid |
			channel_ratio_valid | channel_nominal_valid | channel_range_valid |
			channel_resolution_valid | channel_clipping_valid |
			channel_calibration_valid;
		voltage.phase = Phase::a;
		voltage.quantity = Quantity::voltage;
		voltage.si_unit = SiUnit::volt;
		voltage.storage_bits = 32;
		voltage.valid_bits = 24;
		voltage.gain = {1, 100};
		voltage.offset = {-1, 10};
		voltage.primary_secondary_ratio = {100, 1};
		voltage.nominal = {120, 1};
		voltage.range_minimum = {-170, 1};
		voltage.range_maximum = {170, 1};
		voltage.resolution = {1, 100};
		voltage.clipping_low = -8'388'608;
		voltage.clipping_high = 8'388'607;
		voltage.name = "Va";
		voltage.unit_symbol = "V";
		voltage.description = "phase A voltage";

		ChannelDefinition current = voltage;
		current.stable_id = uuid(0x50);
		current.source_channel = 5;
		current.phase = Phase::b;
		current.quantity = Quantity::current;
		current.si_unit = SiUnit::ampere;
		current.gain = {1, 1000};
		current.offset = {0, 1};
		current.primary_secondary_ratio = {200, 5};
		current.nominal = {5, 1};
		current.range_minimum = {-20, 1};
		current.range_maximum = {20, 1};
		current.resolution = {1, 1000};
		current.name = "Ib";
		current.unit_symbol = "A";
		current.description = "phase B current";
		metadata_.channels = {voltage, current};

		EventDescriptor event;
		event.event_uuid = uuid(0x60);
		event.taxonomy = EventTaxonomy::iec_61000_4_30;
		event.event_type = 7;
		event.lifecycle = EventLifecycle::complete;
		event.time_quality = TimeQuality::locked;
		event.flags = event_start_valid | event_current_valid | event_end_valid |
			event_trigger_valid | event_tai_valid | event_utc_valid |
			event_settings_snapshot_valid | event_contaminated;
		event.phase_mask = 1;
		event.quantity = Quantity::voltage;
		event.si_unit = SiUnit::volt;
		event.trigger_source = 3;
		event.configuration_generation = 4;
		event.severity = 3;
		event.start_sequence = 100;
		event.current_sequence = 102;
		event.end_sequence = 102;
		event.trigger_sequence = 101;
		event.start_tai_nanoseconds = 1'700'000'037'000'000'000ull;
		event.current_tai_nanoseconds = 1'700'000'037'002'000'000ull;
		event.end_tai_nanoseconds = 1'700'000'037'002'000'000ull;
		event.trigger_tai_nanoseconds = 1'700'000'037'001'000'000ull;
		event.start_utc_nanoseconds = 1'700'000'000'000'000'000ull;
		event.current_utc_nanoseconds = 1'700'000'000'002'000'000ull;
		event.end_utc_nanoseconds = 1'700'000'000'002'000'000ull;
		event.trigger_utc_nanoseconds = 1'700'000'000'001'000'000ull;
		event.uncertainty_nanoseconds = 40;
		event.reference_micro_units = 120'000'000;
		event.threshold_micro_units = 108'000'000;
		event.hysteresis_micro_units = 1'000'000;
		event.extrema_micro_units = {95'000'000, 100'000'000, 99'000'000};
		event.duration_samples = 3;
		event.update_count = 1;
		event.status = 9;
		event.taxonomy_name = "voltage-dip";
		event.label = "Voltage dip A";
		event.settings_snapshot_json = R"({"threshold":0.9})";
		metadata_.events.push_back(event);
		metadata_.selected_event_uuid = event.event_uuid;

		metadata_.quality_intervals.push_back({.first_frame = 1,
			.frame_count = 2, .first_sequence = 101, .last_sequence = 102,
			.channel_mask = 1, .flags = quality_clipped | quality_timing_uncertain,
			.severity = 2, .source = 1, .detail_code = 17});
		metadata_.lineage.push_back({.relation = LineageRelation::parent,
			.flags = 0, .related_capture_uuid = uuid(0x70),
			.related_event_uuid = event.event_uuid, .first_sequence = 100,
			.last_sequence = 103, .part_index = 0, .part_count = 1});
		metadata_.source_first_frame = 0;
		metadata_.first_sequence = 100;
		metadata_.last_sequence = 103;
		samples_ = {-2, 100, -1, 101, 0, 102, 1, 103};
	}

	const WaveformMetadata &metadata() const noexcept override { return metadata_; }
	std::uint64_t frame_count() const noexcept override { return 4; }
	std::size_t channel_count() const noexcept override { return 2; }
	std::size_t read_frames(std::uint64_t first_frame,
		std::size_t frame_capacity,
		std::span<std::int64_t> destination) const override
	{
		if (first_frame >= frame_count()) return 0;
		const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(
			frame_capacity, frame_count() - first_frame));
		require(destination.size() >= count * channel_count(),
			"source destination is undersized");
		const auto begin = samples_.begin() + static_cast<std::ptrdiff_t>(
			first_frame * channel_count());
		std::copy_n(begin, count * channel_count(), destination.begin());
		return count;
	}

	WaveformMetadata &mutable_metadata() noexcept { return metadata_; }

private:
	WaveformMetadata metadata_;
	std::vector<std::int64_t> samples_;
};

class DecimatedSource final : public WaveformSource {
public:
	DecimatedSource()
	{
		MemorySource base;
		metadata_ = base.metadata();
		metadata_.channels.resize(1);
		metadata_.timebase_segments = {{
			.first_frame = 0, .frame_count = 3, .first_sequence = 200,
			.sequence_step = 4, .acquisition_rate = {4000, 1},
			.persisted_rate = {1000, 1}, .correlation_sequence = 200,
			.correlation_pl_tick = 600,
			.correlation_tai_nanoseconds = 1'700'000'037'000'000'000ull,
			.correlation_utc_nanoseconds = 1'700'000'000'000'000'000ull,
			.uncertainty_nanoseconds = 40, .decimation_divisor = 4,
			.decimation_method = DecimationMethod::boxcar_mean_toward_zero,
			.clock_source = ClockSource::ptp,
			.time_quality = TimeQuality::locked,
			.flags = time_utc_offset_known, .utc_offset_seconds = -18'000,
			.source_frame_count = 10,
		}};
		auto &event = metadata_.events.front();
		event.start_sequence = 202;
		event.current_sequence = 208;
		event.end_sequence = 208;
		event.trigger_sequence = 202;
		event.start_utc_nanoseconds = 1'700'000'000'000'500'000ull;
		event.current_utc_nanoseconds = 1'700'000'000'002'000'000ull;
		event.end_utc_nanoseconds = event.current_utc_nanoseconds;
		event.trigger_utc_nanoseconds = event.start_utc_nanoseconds;
		metadata_.quality_intervals.clear();
		metadata_.lineage.clear();
		metadata_.source_first_frame = 0;
		metadata_.first_sequence = 200;
		metadata_.last_sequence = 209;
	}

	const WaveformMetadata &metadata() const noexcept override { return metadata_; }
	std::uint64_t frame_count() const noexcept override { return samples_.size(); }
	std::size_t channel_count() const noexcept override { return 1; }
	std::size_t read_frames(std::uint64_t first_frame,
		std::size_t frame_capacity,
		std::span<std::int64_t> destination) const override
	{
		if (first_frame >= samples_.size()) return 0;
		const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(
			frame_capacity, samples_.size() - first_frame));
		require(destination.size() >= count,
			"decimated source destination is undersized");
		std::copy_n(samples_.begin() + static_cast<std::ptrdiff_t>(first_frame),
			count, destination.begin());
		return count;
	}

private:
	WaveformMetadata metadata_;
	std::array<std::int64_t, 3> samples_{10, 20, 30};
};

struct ZipMember {
	std::string name;
	std::span<const std::byte> data;
};

std::vector<ZipMember> zip_members(std::span<const std::byte> bytes)
{
	std::vector<ZipMember> result;
	std::size_t offset = 0;
	while (offset + 4u <= bytes.size() && u32(bytes, offset) == 0x04034b50u) {
		require(offset + 30u <= bytes.size(), "truncated ZIP local header");
		require(u16(bytes, offset + 8u) == 0u, "legacy ZIP member is compressed");
		const auto name_size = u16(bytes, offset + 26u);
		const auto extra_size = u16(bytes, offset + 28u);
		const auto name_offset = offset + 30u;
		require(name_offset + name_size + extra_size <= bytes.size(),
			"truncated ZIP member name");
		const std::string name(reinterpret_cast<const char *>(
			bytes.data() + name_offset), name_size);
		const auto data_offset = name_offset + name_size + extra_size;
		constexpr std::array descriptor_signature{std::byte{0x50},
			std::byte{0x4b}, std::byte{0x07}, std::byte{0x08}};
		const auto descriptor = std::search(bytes.begin() +
			static_cast<std::ptrdiff_t>(data_offset), bytes.end(),
			descriptor_signature.begin(), descriptor_signature.end());
		require(descriptor != bytes.end(), "ZIP data descriptor is missing");
		const auto descriptor_offset = static_cast<std::size_t>(
			descriptor - bytes.begin());
		require(descriptor_offset + 16u <= bytes.size(),
			"truncated ZIP data descriptor");
		const auto size = u32(bytes, descriptor_offset + 8u);
		require(data_offset + size == descriptor_offset,
			"ZIP member size disagrees with descriptor");
		const auto member = bytes.subspan(data_offset, size);
		const auto crc = ::crc32(0L,
			reinterpret_cast<const Bytef *>(member.data()),
			static_cast<uInt>(member.size()));
		require(static_cast<std::uint32_t>(crc) ==
			u32(bytes, descriptor_offset + 4u), "ZIP member CRC mismatch");
		result.push_back({name, member});
		offset = descriptor_offset + 16u;
	}
	require(result.size() == 2u, "legacy ZIP does not contain two members");
	return result;
}

std::vector<std::byte> inflate_record(std::span<const std::byte> compressed)
{
	z_stream stream{};
	require(::inflateInit(&stream) == Z_OK, "initialize PQDIF inflater");
	stream.next_in = reinterpret_cast<Bytef *>(
		const_cast<std::byte *>(compressed.data()));
	stream.avail_in = static_cast<uInt>(compressed.size());
	std::vector<std::byte> output;
	std::array<std::byte, 4096> block{};
	int status = Z_OK;
	while (status == Z_OK) {
		stream.next_out = reinterpret_cast<Bytef *>(block.data());
		stream.avail_out = block.size();
		status = ::inflate(&stream, Z_NO_FLUSH);
		output.insert(output.end(), block.begin(),
			block.begin() + static_cast<std::ptrdiff_t>(block.size() -
				stream.avail_out));
	}
	::inflateEnd(&stream);
	require(status == Z_STREAM_END, "inflate PQDIF record body");
	return output;
}

ConversionOptions options(ExportFormat format)
{
	ConversionOptions value;
	value.format = format;
	value.scope = ExportScope::capture;
	value.maximum_output_bytes = 4u * 1024u * 1024u;
	value.frame_batch_size = 2;
	value.output_stem = "capture 42/unsafe";
	return value;
}

void test_comtrade(MemorySource &source, std::vector<std::byte> &cff)
{
	VectorOutputSink first;
	std::vector<ConversionProgress> progress;
	const auto summary = ComtradeConverter{}.convert(source, first,
		options(ExportFormat::comtrade), {},
		[&](const auto &value) { progress.push_back(value); });
	require(summary.extension == ".cff", "COMTRADE extension");
	require(summary.frames == 4u && summary.bytes == first.bytes().size(),
		"COMTRADE summary");
	require(!progress.empty() && progress.front().processed_frames == 0u &&
		progress.back().processed_frames == 4u, "COMTRADE progress");

	VectorOutputSink second;
	(void)ComtradeConverter{}.convert(source, second,
		options(ExportFormat::comtrade));
	require(first.bytes() == second.bytes(), "COMTRADE output is not deterministic");
	cff = first.bytes();
	const auto all = text(cff);
	require(all.starts_with("--- file type: CFG ---\r\n"), "CFF CFG marker");
	require(all.find("BINARY32\r\n") != std::string::npos, "CFF BINARY32 declaration");
	require(all.find(
		"1.000000000000000000000000000000000000e+03,2\r\n"
		"2.000000000000000000000000000000000000e+03,4\r\n") !=
		std::string::npos,
		"CFF multi-rate table");
	require(all.find("timestamp.rounding=half_even\r\n") != std::string::npos,
		"CFF rounding disclosure");
	require(all.find("channel.1.source_channel=5\r\n") != std::string::npos,
		"CFF source-channel lineage");

	constexpr std::string_view dat_marker = "--- file type: DAT BINARY32: ";
	const auto marker = all.rfind(dat_marker);
	require(marker != std::string::npos, "CFF DAT marker");
	const auto data_offset = all.find(" ---\r\n", marker) + 6u;
	require(data_offset <= cff.size(), "CFF DAT start");
	const auto dat = std::span<const std::byte>{cff}.subspan(data_offset);
	require(dat.size() == 4u * 18u, "CFF DAT geometry");
	const std::array<std::uint32_t, 4> expected_time{
		0u, 1'000'000u, 2'000'000u, 2'500'000u};
	const std::array<std::int32_t, 8> expected_samples{
		-2, 100, -1, 101, 0, 102, 1, 103};
	for (std::size_t frame = 0; frame != 4; ++frame) {
		const auto offset = frame * 18u;
		require(u32(dat, offset) == frame + 1u, "CFF sample number");
		require(u32(dat, offset + 4u) == expected_time[frame],
			"CFF exact multi-rate timestamp");
		for (std::size_t channel = 0; channel != 2; ++channel)
			require(std::bit_cast<std::int32_t>(u32(dat,
				offset + 8u + channel * 4u)) ==
				expected_samples[frame * 2u + channel], "CFF raw sample");
		require(u16(dat, offset + 16u) == (frame < 3u ? 1u : 0u),
			"CFF event status interval");
	}
	require(sha256(cff) ==
		"d5dc2f04ba7359c355dddc1815b3ff7b12788e0b41be450446619ad088617e46",
		std::string("COMTRADE golden vector changed: ") + sha256(cff));
}

void test_comtrade_zip(MemorySource &source,
	std::span<const std::byte> cff, std::vector<std::byte> &zip)
{
	VectorOutputSink sink;
	const auto summary = ComtradeZipConverter{}.convert(source, sink,
		options(ExportFormat::comtrade_zip));
	require(summary.extension == ".zip" && summary.media_type == "application/zip",
		"legacy ZIP summary");
	const auto members = zip_members(sink.bytes());
	require(members[0].name == "capture_42unsafe.cfg", "legacy CFG name");
	require(members[1].name == "capture_42unsafe.dat", "legacy DAT name");
	require(text(members[0].data).find("BINARY32\r\n") != std::string::npos,
		"legacy CFG contents");
	const auto cff_text = text(cff);
	constexpr std::string_view dat_marker = "--- file type: DAT BINARY32: ";
	const auto marker = cff_text.rfind(dat_marker);
	const auto data_offset = cff_text.find(" ---\r\n", marker) + 6u;
	require(std::ranges::equal(members[1].data, cff.subspan(data_offset)),
		"legacy DAT differs from CFF DAT");
	// The ZIP vector pins local headers, CRCs, descriptors and directory order.
	require(sha256(sink.bytes()) ==
		"48689d60b1a2a1dbfd89f39b0cb180b5bf3cea955dbcab14051d8f149adbfb3a",
		std::string("COMTRADE ZIP golden vector changed: ") +
			sha256(sink.bytes()));
	zip = sink.bytes();
}

void test_short_final_boxcar()
{
	DecimatedSource source;
	VectorOutputSink sink;
	(void)ComtradeConverter{}.convert(source, sink,
		options(ExportFormat::comtrade));
	const auto bytes = std::span<const std::byte>{sink.bytes()};
	const auto all = text(bytes);
	constexpr std::string_view dat_marker = "--- file type: DAT BINARY32: ";
	const auto marker = all.rfind(dat_marker);
	require(marker != std::string::npos, "decimated CFF DAT marker");
	const auto data_offset = all.find(" ---\r\n", marker) + 6u;
	const auto dat = bytes.subspan(data_offset);
	require(dat.size() == 3u * 14u, "decimated CFF DAT geometry");
	for (std::size_t frame = 0; frame != 3; ++frame)
		require(u16(dat, frame * 14u + 12u) == 1u,
			"event projection includes intersecting short boxcar groups");

	VectorOutputSink pqdif;
	(void)PqdifConverter{}.convert(source, pqdif,
		options(ExportFormat::pqdif));
	require(!pqdif.bytes().empty(), "decimated PQDIF conversion");
}

void test_incomplete_event_status()
{
	MemorySource source;
	auto &event = source.mutable_metadata().events.front();
	event.flags &= ~event_end_valid;
	event.lifecycle = EventLifecycle::update;
	VectorOutputSink sink;
	(void)ComtradeConverter{}.convert(source, sink,
		options(ExportFormat::comtrade));
	const auto bytes = std::span<const std::byte>{sink.bytes()};
	const auto all = text(bytes);
	constexpr std::string_view dat_marker = "--- file type: DAT BINARY32: ";
	const auto marker = all.rfind(dat_marker);
	const auto data_offset = all.find(" ---\r\n", marker) + 6u;
	const auto dat = bytes.subspan(data_offset);
	require(u16(dat, 3u * 18u + 16u) == 1u,
		"incomplete event remains active through the selected interval");
}

void test_pqdif(MemorySource &source, std::vector<std::byte> &pqdif)
{
	VectorOutputSink first;
	auto requested = options(ExportFormat::pqdif);
	const auto summary = PqdifConverter{}.convert(source, first, requested);
	require(summary.extension == ".pqd" && summary.frames == 4u,
		"PQDIF summary");
	VectorOutputSink second;
	(void)PqdifConverter{}.convert(source, second, requested);
	require(first.bytes() == second.bytes(), "PQDIF output is not deterministic");

	const auto bytes = std::span<const std::byte>{first.bytes()};
	std::size_t offset = 0;
	std::size_t record_count = 0;
	std::vector<std::vector<std::byte>> logical_bodies;
	while (offset != bytes.size()) {
		require(offset + 64u <= bytes.size(), "truncated PQDIF header");
		require(u32(bytes, offset + 32u) == 64u, "PQDIF header size");
		const auto body_size = u32(bytes, offset + 36u);
		const auto next = u32(bytes, offset + 40u);
		const auto checksum = u32(bytes, offset + 44u);
		require(offset + 64u + body_size <= bytes.size(), "truncated PQDIF body");
		const auto body = bytes.subspan(offset + 64u, body_size);
		auto body_checksum = ::adler32(0L, Z_NULL, 0);
		body_checksum = ::adler32(body_checksum,
			reinterpret_cast<const Bytef *>(body.data()),
			static_cast<uInt>(body.size()));
		require(static_cast<std::uint32_t>(body_checksum) == checksum,
			"PQDIF Adler-32");
		logical_bodies.push_back(record_count == 0u
			? std::vector<std::byte>{body.begin(), body.end()}
			: inflate_record(body));
		const auto expected_next = offset + 64u + body_size;
		if (expected_next == bytes.size())
			require(next == 0u, "last PQDIF next-record link");
		else
			require(next == expected_next, "PQDIF next-record link");
		offset = expected_next;
		++record_count;
	}
	require(record_count == 4u, "PQDIF record count");
	const std::array<std::byte, 16> raw_pattern{
		std::byte{0xfe}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff},
		std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff},
		std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
		std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
	require(std::search(logical_bodies.back().begin(), logical_bodies.back().end(),
		raw_pattern.begin(), raw_pattern.end()) != logical_bodies.back().end(),
		"PQDIF raw signed waveform series");

	const auto generated = uuid_v5(uuid(0x20), "stable-object");
	require((std::to_integer<unsigned>(generated[6]) & 0xf0u) == 0x50u,
		"UUIDv5 version bits");
	require((std::to_integer<unsigned>(generated[8]) & 0xc0u) == 0x80u,
		"UUIDv5 variant bits");
	require(sha256(bytes) ==
		"3bc09610633c1693f075b0f57d16e73f54918e46a56a0d312f0d76ff473b8933",
		std::string("PQDIF golden vector changed: ") + sha256(bytes));
	pqdif = first.bytes();
}

void write_vector(const std::filesystem::path &path,
	std::span<const std::byte> bytes);

void test_optional_identity()
{
	MemorySource source;
	auto &metadata = source.mutable_metadata();
	metadata.capture.station_name.clear();
	metadata.capture.site_name.clear();
	metadata.capture.circuit_name.clear();
	metadata.capture.device_serial.clear();
	metadata.capture.calibration_id.clear();
	metadata.capture.calibration_status = CalibrationStatus::unknown;
	for (auto &channel : metadata.channels)
		channel.flags &= ~channel_calibration_valid;
	VectorOutputSink cff;
	(void)ComtradeConverter{}.convert(source, cff, options(ExportFormat::comtrade));
	const auto content = text(cff.bytes());
	require(content.find("\r\n," + metadata.capture.device_model + ",2013\r\n") != std::string::npos,
		"COMTRADE preserves blank station and uses captured model without inventing serial");
	require(content.find("calibration.id=\r\ncalibration.status=0\r\n") != std::string::npos,
		"COMTRADE discloses unknown calibration");
	VectorOutputSink zip;
	(void)ComtradeZipConverter{}.convert(source, zip, options(ExportFormat::comtrade_zip));
	require(!zip.bytes().empty(), "legacy ZIP allows optional identity");
	VectorOutputSink pqdif;
	(void)PqdifConverter{}.convert(source, pqdif, options(ExportFormat::pqdif));
	const auto bytes = std::span<const std::byte>{pqdif.bytes()};
	const auto source_offset = u32(bytes, 40);
	const auto settings_offset = u32(bytes, source_offset + 40);
	const auto settings_body = inflate_record(bytes.subspan(settings_offset + 64,
		u32(bytes, settings_offset + 36)));
	const std::array<std::byte, 16> calibration_tag{
		std::byte{0x80}, std::byte{0x81}, std::byte{0xf2}, std::byte{0x62},
		std::byte{0xc4}, std::byte{0xf9}, std::byte{0xcf}, std::byte{0x11},
		std::byte{0x9d}, std::byte{0x89}, std::byte{0x00}, std::byte{0x80},
		std::byte{0xc7}, std::byte{0x2e}, std::byte{0x70}, std::byte{0xa3}};
	const auto found = std::search(settings_body.begin(), settings_body.end(),
		calibration_tag.begin(), calibration_tag.end());
	require(found != settings_body.end(), "PQDIF retains required use-calibration tag");
	const auto tag_offset = static_cast<std::size_t>(found - settings_body.begin());
	require(u32(settings_body, tag_offset + 20) == 0,
		"PQDIF does not assert calibration for unknown channels");
	require(text(settings_body).find("calibration.status=0\ncalibration.id=") != std::string::npos,
		"PQDIF preserves unknown calibration provenance");
	if (const auto *directory = std::getenv("MNC_WAVEFORM_GOLDEN_DIR")) {
		std::filesystem::create_directories(directory);
		write_vector(std::filesystem::path(directory) / "unprovisioned.cff", cff.bytes());
		write_vector(std::filesystem::path(directory) / "unprovisioned.zip", zip.bytes());
		write_vector(std::filesystem::path(directory) / "unprovisioned.pqd", pqdif.bytes());
	}
}

void write_vector(const std::filesystem::path &path,
	std::span<const std::byte> bytes)
{
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	require(output.is_open(), "open generated interoperability vector");
	output.write(reinterpret_cast<const char *>(bytes.data()),
		static_cast<std::streamsize>(bytes.size()));
	require(output.good(), "write generated interoperability vector");
}

void expect_code(auto &&operation, ConversionErrorCode expected,
	std::string_view message)
{
	try {
		operation();
	} catch (const ConversionError &error) {
		require(error.code() == expected, message);
		return;
	}
	throw std::runtime_error(std::string(message));
}

void test_failures(MemorySource &source)
{
	VectorOutputSink tiny(16);
	expect_code([&] {
		(void)ComtradeConverter{}.convert(source, tiny,
			options(ExportFormat::comtrade));
	}, ConversionErrorCode::output_too_large, "COMTRADE size limit");

	std::stop_source cancellation;
	cancellation.request_stop();
	VectorOutputSink cancelled_output;
	expect_code([&] {
		(void)PqdifConverter{}.convert(source, cancelled_output,
			options(ExportFormat::pqdif), cancellation.get_token());
	}, ConversionErrorCode::conversion_cancelled, "PQDIF cancellation");

	auto &metadata = source.mutable_metadata();
	const auto original_flags = metadata.timebase_segments[1].flags;
	metadata.timebase_segments[1].flags |= time_sequence_gap_before;
	VectorOutputSink gap_output;
	expect_code([&] {
		(void)ComtradeConverter{}.convert(source, gap_output,
			options(ExportFormat::comtrade));
	}, ConversionErrorCode::source_discontinuity_unsupported, "gap rejection");
	metadata.timebase_segments[1].flags = original_flags;

	const auto original_denominator = metadata.channels[0].gain.denominator;
	metadata.channels[0].gain.denominator = 0;
	VectorOutputSink malformed_output;
	expect_code([&] {
		(void)ComtradeConverter{}.convert(source, malformed_output,
			options(ExportFormat::comtrade));
	}, ConversionErrorCode::source_not_ready, "malformed transform rejection");
	metadata.channels[0].gain.denominator = original_denominator;

	const auto original_rate = metadata.timebase_segments[0].persisted_rate;
	metadata.timebase_segments[0].persisted_rate.numerator = 0;
	VectorOutputSink malformed_timebase_output;
	expect_code([&] {
		(void)ComtradeConverter{}.convert(source, malformed_timebase_output,
			options(ExportFormat::comtrade));
	}, ConversionErrorCode::source_invalid, "malformed timebase rejection");
	metadata.timebase_segments[0].persisted_rate = original_rate;

	auto event_options = options(ExportFormat::pqdif);
	event_options.scope = ExportScope::event;
	event_options.selected_event_uuid = uuid(0xf0);
	VectorOutputSink missing_event_output;
	expect_code([&] {
		(void)PqdifConverter{}.convert(source, missing_event_output, event_options);
	}, ConversionErrorCode::source_event_not_found, "missing selected event");

	const auto original_sample_flags = metadata.quality_intervals[0].flags;
	metadata.quality_intervals[0].flags |= quality_transport_loss;
	VectorOutputSink loss_output;
	expect_code([&] {
		(void)PqdifConverter{}.convert(source, loss_output,
			options(ExportFormat::pqdif));
	}, ConversionErrorCode::source_discontinuity_unsupported,
		"transport-loss rejection");
	metadata.quality_intervals[0].flags = original_sample_flags;
}

} // namespace

int main()
{
	try {
		MemorySource source;
		std::vector<std::byte> cff;
		std::vector<std::byte> zip;
		std::vector<std::byte> pqdif;
		test_comtrade(source, cff);
		test_comtrade_zip(source, cff, zip);
		test_short_final_boxcar();
		test_incomplete_event_status();
		test_pqdif(source, pqdif);
		test_optional_identity();
		test_failures(source);
		if (const auto *directory = std::getenv("MNC_WAVEFORM_GOLDEN_DIR")) {
			std::filesystem::create_directories(directory);
			write_vector(std::filesystem::path(directory) / "waveform.cff", cff);
			write_vector(std::filesystem::path(directory) / "waveform.zip", zip);
			write_vector(std::filesystem::path(directory) / "waveform.pqd", pqdif);
		}
		std::cout << "PASS: waveform_converter_test\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "waveform_converter_test: " << error.what() << '\n';
		return 1;
	}
}
