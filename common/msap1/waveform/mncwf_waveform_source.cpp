#include "msap1/waveform/mncwf_waveform_source.hpp"

#include <zstd.h>

#include <algorithm>
#include <bit>
#include <cerrno>
#include <cstring>
#include <limits>
#include <mutex>
#include <ranges>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace msap1 {
namespace {

using namespace mnc::waveform;

class FileDescriptor final {
public:
	explicit FileDescriptor(int value = -1) : value_(value) {}
	~FileDescriptor()
	{
		if (value_ >= 0)
			::close(value_);
	}
	FileDescriptor(const FileDescriptor &) = delete;
	FileDescriptor &operator=(const FileDescriptor &) = delete;
	FileDescriptor(FileDescriptor &&other) noexcept
		: value_(std::exchange(other.value_, -1)) {}
	FileDescriptor &operator=(FileDescriptor &&other) noexcept
	{
		if (this != &other) {
			if (value_ >= 0)
				::close(value_);
			value_ = std::exchange(other.value_, -1);
		}
		return *this;
	}
	[[nodiscard]] int get() const noexcept { return value_; }

private:
	int value_;
};

class Mapping final {
public:
	explicit Mapping(int source)
	{
		const int duplicated = ::fcntl(source, F_DUPFD_CLOEXEC, 3);
		if (duplicated < 0)
			throw std::system_error(errno, std::generic_category(),
				"duplicate MNCWF descriptor");
		file_ = FileDescriptor(duplicated);
		struct stat status {};
		if (::fstat(file_.get(), &status) != 0)
			throw std::system_error(errno, std::generic_category(),
				"inspect MNCWF descriptor");
		if (!S_ISREG(status.st_mode) || status.st_size <= 0)
			throw ConversionError(ConversionErrorCode::source_invalid,
				"MNCWF source is not a non-empty regular file");
		const auto size = static_cast<std::uint64_t>(status.st_size);
		if (size > mncwf_v4_max_file_bytes ||
		    size > std::numeric_limits<std::size_t>::max())
			throw ConversionError(ConversionErrorCode::source_invalid,
				"MNCWF source exceeds its file-size bound");
		size_ = static_cast<std::size_t>(size);
		address_ = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, file_.get(), 0);
		if (address_ == MAP_FAILED) {
			address_ = nullptr;
			throw std::system_error(errno, std::generic_category(),
				"map MNCWF descriptor");
		}
	}

	~Mapping()
	{
		if (address_)
			::munmap(address_, size_);
	}
	Mapping(const Mapping &) = delete;
	Mapping &operator=(const Mapping &) = delete;
	Mapping(Mapping &&) = delete;

	[[nodiscard]] std::span<const std::byte> bytes() const noexcept
	{
		return {static_cast<const std::byte *>(address_), size_};
	}

private:
	FileDescriptor file_;
	void *address_ = nullptr;
	std::size_t size_ = 0;
};

MncwfValidationMode validation_mode(std::span<const std::byte> bytes)
{
	if (bytes.size() < 12u)
		return MncwfValidationMode::complete;
	std::uint32_t version = 0;
	for (unsigned index = 0; index != 4; ++index)
		version |= std::to_integer<std::uint32_t>(bytes[8u + index]) <<
			(index * 8u);
	return version == mncwf_v5_version
		? MncwfValidationMode::metadata_only : MncwfValidationMode::complete;
}

MncwfV4SelectionDescriptor capture_selection(const MncwfV4Reader &reader)
{
	MncwfV4SelectionDescriptor result;
	result.capture_metadata = reader.capture_metadata();
	result.timebase_segments = reader.timebase_segments();
	result.events = reader.events();
	result.quality_intervals = reader.quality_intervals();
	result.lineage = reader.lineage();
	result.frame_count = reader.sample_frame_count();
	result.first_sequence = result.timebase_segments.front().first_sequence;
	const auto &last = result.timebase_segments.back();
	if (last.source_frame_count == 0 || last.first_sequence >
	    std::numeric_limits<std::uint64_t>::max() - last.source_frame_count + 1u)
		throw ConversionError(ConversionErrorCode::source_invalid,
			"MNCWF source sequence range overflows");
	result.last_sequence = last.first_sequence + last.source_frame_count - 1u;
	return result;
}

template <class To, class From>
To same_enum(From value)
{
	return static_cast<To>(static_cast<std::underlying_type_t<From>>(value));
}

WaveformMetadata convert_metadata(const MncwfV4Reader &reader,
	const MncwfV4SelectionDescriptor &selection, ExportScope scope)
{
	WaveformMetadata result;
	const auto &source_capture = selection.capture_metadata;
	auto &capture = result.capture;
	capture.capture_uuid = source_capture.capture_uuid;
	capture.source_capture_uuid = reader.capture_metadata().capture_uuid;
	capture.device_uuid = source_capture.device_uuid;
	capture.configuration_sha256 = source_capture.configuration_sha256;
	capture.sensor_profile_sha256 = source_capture.sensor_profile_sha256;
	capture.created_tai_nanoseconds = source_capture.created_tai_nanoseconds;
	capture.created_utc_nanoseconds = source_capture.created_utc_nanoseconds;
	capture.nominal_voltage = {source_capture.nominal_voltage_numerator,
		source_capture.nominal_voltage_denominator};
	capture.nominal_frequency = {source_capture.nominal_frequency_numerator,
		source_capture.nominal_frequency_denominator};
	capture.topology = same_enum<Topology>(source_capture.topology);
	capture.calibration_status = same_enum<CalibrationStatus>(
		source_capture.calibration_status);
	capture.flags = source_capture.flags;
	capture.station_name = source_capture.station_name;
	capture.site_name = source_capture.site_name;
	capture.circuit_name = source_capture.circuit_name;
	capture.product_name = source_capture.product_name;
	capture.device_model = source_capture.device_model;
	capture.firmware_version = source_capture.firmware_version;
	capture.software_build_id = source_capture.software_build_id;
	capture.sensor_profile_id = source_capture.sensor_profile_id;
	capture.configuration_id = source_capture.configuration_id;
	capture.calibration_id = source_capture.calibration_id;
	capture.device_serial = source_capture.device_serial;
	capture.comments = source_capture.comments;

	result.timebase_segments.reserve(selection.timebase_segments.size());
	for (const auto &source : selection.timebase_segments) {
		TimebaseSegment destination;
		destination.first_frame = source.first_frame;
		destination.frame_count = source.frame_count;
		destination.first_sequence = source.first_sequence;
		destination.sequence_step = source.sequence_step;
		destination.acquisition_rate = {source.acquisition_rate_numerator,
			source.acquisition_rate_denominator};
		destination.persisted_rate = {source.persisted_rate_numerator,
			source.persisted_rate_denominator};
		destination.correlation_sequence = source.correlation_sequence;
		destination.correlation_pl_tick = source.correlation_pl_tick;
		destination.correlation_tai_nanoseconds =
			source.correlation_tai_nanoseconds;
		destination.correlation_utc_nanoseconds =
			source.correlation_utc_nanoseconds;
		destination.uncertainty_nanoseconds = source.uncertainty_nanoseconds;
		destination.decimation_divisor = source.decimation_divisor;
		destination.decimation_method = same_enum<DecimationMethod>(
			source.decimation_method);
		destination.clock_source = same_enum<ClockSource>(source.clock_source);
		destination.time_quality = same_enum<TimeQuality>(source.time_quality);
		destination.flags = source.flags;
		destination.utc_offset_seconds = source.utc_offset_seconds;
		destination.source_frame_count = source.source_frame_count;
		result.timebase_segments.push_back(destination);
	}

	result.channels.reserve(reader.channels().size());
	for (const auto &source : reader.channels()) {
		ChannelDefinition destination;
		destination.stable_id = source.stable_id;
		destination.source_channel = source.source_channel;
		destination.flags = source.flags;
		destination.phase = same_enum<Phase>(source.phase);
		destination.quantity = same_enum<Quantity>(source.quantity);
		destination.si_unit = same_enum<SiUnit>(source.si_unit);
		destination.storage_bits = source.storage_bits;
		destination.valid_bits = source.valid_bits;
		destination.display_exponent10 = source.display_exponent10;
		destination.gain = {source.gain_numerator, source.gain_denominator};
		destination.offset = {source.offset_numerator, source.offset_denominator};
		destination.primary_secondary_ratio = {
			source.primary_secondary_ratio_numerator,
			source.primary_secondary_ratio_denominator};
		destination.nominal = {source.nominal_numerator,
			source.nominal_denominator};
		destination.range_minimum = {source.range_minimum_numerator,
			source.range_minimum_denominator};
		destination.range_maximum = {source.range_maximum_numerator,
			source.range_maximum_denominator};
		destination.resolution = {source.resolution_numerator,
			source.resolution_denominator};
		destination.clipping_low = source.clipping_low;
		destination.clipping_high = source.clipping_high;
		destination.name = source.name;
		destination.unit_symbol = source.unit_symbol;
		destination.description = source.description;
		result.channels.push_back(std::move(destination));
	}

	result.events.reserve(selection.events.size());
	for (const auto &source : selection.events) {
		EventDescriptor destination;
		destination.event_uuid = source.event_uuid;
		destination.taxonomy = same_enum<EventTaxonomy>(source.taxonomy);
		destination.event_type = source.event_type;
		destination.lifecycle = same_enum<EventLifecycle>(source.lifecycle);
		destination.time_quality = same_enum<TimeQuality>(source.time_quality);
		destination.flags = source.flags;
		destination.phase_mask = source.phase_mask;
		destination.quantity = same_enum<Quantity>(source.quantity);
		destination.si_unit = same_enum<SiUnit>(source.si_unit);
		destination.trigger_source = source.trigger_source;
		destination.configuration_generation = source.configuration_generation;
		destination.severity = source.severity;
		destination.start_sequence = source.start_sequence;
		destination.current_sequence = source.current_sequence;
		destination.end_sequence = source.end_sequence;
		destination.trigger_sequence = source.trigger_sequence;
		destination.start_tai_nanoseconds = source.start_tai_nanoseconds;
		destination.current_tai_nanoseconds = source.current_tai_nanoseconds;
		destination.end_tai_nanoseconds = source.end_tai_nanoseconds;
		destination.trigger_tai_nanoseconds = source.trigger_tai_nanoseconds;
		destination.start_utc_nanoseconds = source.start_utc_nanoseconds;
		destination.current_utc_nanoseconds = source.current_utc_nanoseconds;
		destination.end_utc_nanoseconds = source.end_utc_nanoseconds;
		destination.trigger_utc_nanoseconds = source.trigger_utc_nanoseconds;
		destination.uncertainty_nanoseconds = source.uncertainty_nanoseconds;
		destination.reference_micro_units = source.reference_micro_units;
		destination.threshold_micro_units = source.threshold_micro_units;
		destination.hysteresis_micro_units = source.hysteresis_micro_units;
		destination.extrema_micro_units = source.extrema_micro_units;
		destination.duration_samples = source.duration_samples;
		destination.update_count = source.update_count;
		destination.status = source.status;
		destination.taxonomy_name = source.taxonomy_name;
		destination.label = source.label;
		destination.settings_snapshot_json = source.settings_snapshot_json;
		result.events.push_back(std::move(destination));
	}

	result.quality_intervals.reserve(selection.quality_intervals.size());
	for (const auto &source : selection.quality_intervals)
		result.quality_intervals.push_back({source.first_frame,
			source.frame_count, source.first_sequence, source.last_sequence,
			source.channel_mask, source.flags, source.severity, source.source,
			source.detail_code});
	result.lineage.reserve(selection.lineage.size());
	for (const auto &source : selection.lineage)
		result.lineage.push_back({same_enum<LineageRelation>(source.relation),
			source.flags, source.related_capture_uuid, source.related_event_uuid,
			source.first_sequence, source.last_sequence, source.part_index,
			source.part_count});
	if (scope == ExportScope::event)
		result.selected_event_uuid = selection.selected_event_uuid;
	result.source_first_frame = selection.source_first_frame;
	result.first_sequence = selection.first_sequence;
	result.last_sequence = selection.last_sequence;
	return result;
}

std::int64_t decode_signed(std::span<const std::byte> bytes,
	std::uint16_t bits)
{
	std::uint64_t raw = 0;
	for (std::size_t index = 0; index != bytes.size(); ++index)
		raw |= std::to_integer<std::uint64_t>(bytes[index]) << (index * 8u);
	if (bits < 64u) {
		const auto mask = (std::uint64_t{1} << bits) - 1u;
		raw &= mask;
		if ((raw & (std::uint64_t{1} << (bits - 1u))) != 0u)
			raw |= ~mask;
	}
	return std::bit_cast<std::int64_t>(raw);
}

} // namespace

struct MncwfWaveformSource::Impl {
	Impl(int descriptor, ExportFormat format, ExportScope scope,
		std::optional<MncwfUuid> event_uuid)
		: mapping(descriptor),
		  reader(mapping.bytes(), validation_mode(mapping.bytes())),
		  selection(scope == ExportScope::capture
			  ? capture_selection(reader)
			  : event_uuid
				? make_mncwf_v4_event_selection(reader, *event_uuid)
				: throw ConversionError(ConversionErrorCode::invalid_options,
					"event conversion requires an event UUID")),
		  converted(convert_metadata(reader, selection, scope))
	{
		const auto readiness = assess_mncwf_v4_conversion_readiness(reader);
		const auto &missing = format == ExportFormat::pqdif
			? readiness.pqdif_missing : readiness.comtrade_missing;
		if (!missing.empty())
			throw ConversionError(ConversionErrorCode::source_not_ready,
				"MNCWF source is missing conversion metadata", missing);

		const auto sample = std::ranges::find_if(reader.sections(),
			[](const auto &section) {
				return section.type == static_cast<std::uint32_t>(
					MncwfV4SectionType::sample_data);
			});
		if (sample == reader.sections().end())
			throw ConversionError(ConversionErrorCode::source_invalid,
				"MNCWF sample section is missing");
		sample_section = sample->data;
		if (mncwf_crc32c(sample_section) != sample->crc32c)
			throw ConversionError(ConversionErrorCode::source_invalid,
				"MNCWF sample-section CRC32C mismatch");

		std::size_t offset = 0;
		channel_offsets.reserve(reader.channels().size());
		for (const auto &channel : reader.channels()) {
			channel_offsets.push_back(offset);
			offset += channel.storage_bits / 8u;
		}
		if (offset != reader.sample_frame_bytes())
			throw ConversionError(ConversionErrorCode::source_invalid,
				"MNCWF channel geometry disagrees with its frame size");
	}

	[[nodiscard]] std::span<const std::byte> absolute_frame(
		std::uint64_t absolute) const
	{
		if (reader.version() == mncwf_v4_version)
			return reader.sample_frame(absolute);
		const auto found = std::ranges::find_if(reader.sample_chunks(),
			[absolute](const auto &chunk) {
				return absolute >= chunk.first_frame &&
					absolute - chunk.first_frame < chunk.frame_count;
			});
		if (found == reader.sample_chunks().end())
			throw ConversionError(ConversionErrorCode::source_invalid,
				"MNCWF v5 frame is not covered by a sample chunk");
		const auto chunk_index = static_cast<std::size_t>(
			found - reader.sample_chunks().begin());
		if (cached_chunk != chunk_index) {
			cache.resize(static_cast<std::size_t>(found->logical_bytes));
			const auto stored = sample_section.subspan(
				static_cast<std::size_t>(found->stored_offset),
				static_cast<std::size_t>(found->stored_bytes));
			if (found->codec == MncwfChunkCodec::raw)
				std::copy(stored.begin(), stored.end(), cache.begin());
			else {
				ZSTD_DCtx *context = ZSTD_createDCtx();
				if (!context)
					throw ConversionError(ConversionErrorCode::internal_error,
						"cannot allocate bounded MNCWF zstd context");
				const auto bound = ZSTD_DCtx_setParameter(context,
					ZSTD_d_windowLogMax, 20);
				const auto produced = ZSTD_isError(bound) ? bound :
					ZSTD_decompressDCtx(context, cache.data(), cache.size(),
						stored.data(), stored.size());
				ZSTD_freeDCtx(context);
				if (ZSTD_isError(produced) || produced != cache.size())
					throw ConversionError(ConversionErrorCode::source_invalid,
						"MNCWF v5 sample chunk decompression failed");
			}
			if (mncwf_crc32c(cache) != found->logical_crc32c)
				throw ConversionError(ConversionErrorCode::source_invalid,
					"MNCWF v5 sample chunk logical CRC32C mismatch");
			cached_chunk = chunk_index;
		}
		const auto local = absolute - found->first_frame;
		return std::span<const std::byte>{cache}.subspan(
			static_cast<std::size_t>(local * reader.sample_frame_bytes()),
			reader.sample_frame_bytes());
	}

	Mapping mapping;
	MncwfV4Reader reader;
	MncwfV4SelectionDescriptor selection;
	WaveformMetadata converted;
	std::span<const std::byte> sample_section;
	std::vector<std::size_t> channel_offsets;
	mutable std::mutex read_mutex;
	mutable std::size_t cached_chunk = std::numeric_limits<std::size_t>::max();
	mutable std::vector<std::byte> cache;
};

MncwfWaveformSource::MncwfWaveformSource(int descriptor, ExportFormat format,
	ExportScope scope, std::optional<MncwfUuid> event_uuid)
	: impl_(std::make_unique<Impl>(descriptor, format, scope, event_uuid))
{
}

MncwfWaveformSource::~MncwfWaveformSource() = default;

const WaveformMetadata &MncwfWaveformSource::metadata() const noexcept
{
	return impl_->converted;
}

std::uint64_t MncwfWaveformSource::frame_count() const noexcept
{
	return impl_->selection.frame_count;
}

std::size_t MncwfWaveformSource::channel_count() const noexcept
{
	return impl_->reader.channels().size();
}

std::size_t MncwfWaveformSource::read_frames(std::uint64_t first_frame,
	std::size_t frame_capacity, std::span<std::int64_t> destination) const
{
	if (first_frame > frame_count())
		throw ConversionError(ConversionErrorCode::source_invalid,
			"MNCWF read begins beyond the selected interval");
	if (frame_capacity > destination.size() / channel_count())
		throw ConversionError(ConversionErrorCode::source_invalid,
			"MNCWF read destination is smaller than its declared capacity");
	const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(
		frame_capacity, frame_count() - first_frame));
	if (count == 0)
		return 0;
	std::lock_guard lock(impl_->read_mutex);
	for (std::size_t local = 0; local != count; ++local) {
		const auto frame = impl_->absolute_frame(
			impl_->selection.source_first_frame + first_frame + local);
		for (std::size_t channel = 0; channel != channel_count(); ++channel) {
			const auto &definition = impl_->reader.channels()[channel];
			const auto bytes = definition.storage_bits / 8u;
			destination[local * channel_count() + channel] = decode_signed(
				frame.subspan(impl_->channel_offsets[channel], bytes),
				definition.storage_bits);
		}
	}
	return count;
}

std::uint32_t MncwfWaveformSource::source_version() const noexcept
{
	return impl_->reader.version();
}

} // namespace msap1
