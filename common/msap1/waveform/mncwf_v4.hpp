#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace msap1 {

inline constexpr std::array<std::byte, 8> mncwf_magic{
	std::byte{'M'}, std::byte{'N'}, std::byte{'C'}, std::byte{'W'},
	std::byte{'F'}, std::byte{'1'}, std::byte{0}, std::byte{0}};
inline constexpr std::uint32_t mncwf_v4_version = 4;
inline constexpr std::size_t mncwf_v4_header_bytes = 64;
inline constexpr std::size_t mncwf_v4_directory_entry_bytes = 56;
inline constexpr std::size_t mncwf_v4_section_header_bytes = 48;
inline constexpr std::size_t mncwf_v4_capture_metadata_bytes = 256;
inline constexpr std::size_t mncwf_v4_timebase_segment_bytes = 128;
inline constexpr std::size_t mncwf_v4_channel_definition_bytes = 208;
inline constexpr std::size_t mncwf_v4_event_descriptor_bytes = 256;
inline constexpr std::size_t mncwf_v4_quality_interval_bytes = 64;
inline constexpr std::size_t mncwf_v4_lineage_entry_bytes = 64;

inline constexpr std::size_t mncwf_v4_mandatory_section_count = 7;
inline constexpr std::size_t mncwf_v4_max_sections = 64;
inline constexpr std::size_t mncwf_v4_max_channels = 64;
inline constexpr std::size_t mncwf_v4_max_events = 4096;
inline constexpr std::size_t mncwf_v4_max_lineage_entries = 4096;
inline constexpr std::size_t mncwf_v4_max_string_bytes = 64u * 1024u;
inline constexpr std::uint64_t mncwf_v4_max_file_bytes =
	512ull * 1024ull * 1024ull;
inline constexpr std::uint64_t mncwf_v4_max_metadata_section_bytes =
	16ull * 1024ull * 1024ull;

inline constexpr std::uint16_t mncwf_v4_section_required = 1u << 0u;
inline constexpr std::uint16_t mncwf_v4_known_section_flags =
	mncwf_v4_section_required;

enum class MncwfV4SectionType : std::uint32_t {
	capture_metadata = 1,
	timebase_segments = 2,
	channel_definitions = 3,
	event_descriptors = 4,
	quality_intervals = 5,
	lineage = 6,
	sample_data = 7,
};

enum class MncwfTopology : std::uint32_t {
	unknown = 0,
	wye = 1,
	delta = 2,
};

enum class MncwfCalibrationStatus : std::uint32_t {
	unknown = 0,
	valid = 1,
	expired = 2,
	invalid = 3,
};

enum class MncwfDecimationMethod : std::uint16_t {
	none = 0,
	boxcar_mean_toward_zero = 1,
};

enum class MncwfClockSource : std::uint16_t {
	unknown = 0,
	system = 1,
	ptp = 2,
	gnss = 3,
	manual = 4,
};

enum class MncwfTimeQuality : std::uint16_t {
	unknown = 0,
	unlocked = 1,
	holdover = 2,
	locked = 3,
};

inline constexpr std::uint16_t mncwf_time_utc_offset_known = 1u << 0u;
inline constexpr std::uint16_t mncwf_time_positive_leap_pending = 1u << 1u;
inline constexpr std::uint16_t mncwf_time_negative_leap_pending = 1u << 2u;
inline constexpr std::uint16_t mncwf_time_rate_change_before = 1u << 3u;
inline constexpr std::uint16_t mncwf_time_sequence_gap_before = 1u << 4u;
inline constexpr std::uint16_t mncwf_time_known_flags =
	mncwf_time_utc_offset_known | mncwf_time_positive_leap_pending |
	mncwf_time_negative_leap_pending | mncwf_time_rate_change_before |
	mncwf_time_sequence_gap_before;

enum class MncwfPhase : std::uint16_t {
	none = 0,
	a = 1,
	b = 2,
	c = 3,
	neutral = 4,
	ab = 5,
	bc = 6,
	ca = 7,
};

enum class MncwfQuantity : std::uint16_t {
	unknown = 0,
	current = 1,
	voltage = 2,
	status = 3,
	frequency = 4,
	ratio = 5,
};

enum class MncwfSiUnit : std::uint16_t {
	dimensionless = 0,
	ampere = 1,
	volt = 2,
	hertz = 3,
};

enum class MncwfSampleEncoding : std::uint16_t {
	signed_integer_little_endian = 1,
};

inline constexpr std::uint32_t mncwf_channel_enabled = 1u << 0u;
inline constexpr std::uint32_t mncwf_channel_transform_valid = 1u << 1u;
inline constexpr std::uint32_t mncwf_channel_ratio_valid = 1u << 2u;
inline constexpr std::uint32_t mncwf_channel_nominal_valid = 1u << 3u;
inline constexpr std::uint32_t mncwf_channel_range_valid = 1u << 4u;
inline constexpr std::uint32_t mncwf_channel_resolution_valid = 1u << 5u;
inline constexpr std::uint32_t mncwf_channel_clipping_valid = 1u << 6u;
inline constexpr std::uint32_t mncwf_channel_calibration_valid = 1u << 7u;
inline constexpr std::uint32_t mncwf_channel_known_flags =
	mncwf_channel_enabled | mncwf_channel_transform_valid |
	mncwf_channel_ratio_valid | mncwf_channel_nominal_valid |
	mncwf_channel_range_valid | mncwf_channel_resolution_valid |
	mncwf_channel_clipping_valid | mncwf_channel_calibration_valid;

enum class MncwfEventTaxonomy : std::uint16_t {
	unknown = 0,
	iec_61000_4_30 = 1,
	product_alarm = 2,
};

enum class MncwfEventLifecycle : std::uint16_t {
	start = 1,
	update = 2,
	end = 3,
	abort = 4,
	complete = 5,
};

inline constexpr std::uint32_t mncwf_event_phase_a = 1u << 0u;
inline constexpr std::uint32_t mncwf_event_phase_b = 1u << 1u;
inline constexpr std::uint32_t mncwf_event_phase_c = 1u << 2u;
inline constexpr std::uint32_t mncwf_event_phase_neutral = 1u << 3u;
inline constexpr std::uint32_t mncwf_event_phase_system = 1u << 4u;
inline constexpr std::uint32_t mncwf_event_known_phase_mask =
	mncwf_event_phase_a | mncwf_event_phase_b | mncwf_event_phase_c |
	mncwf_event_phase_neutral | mncwf_event_phase_system;

inline constexpr std::uint32_t mncwf_event_start_valid = 1u << 0u;
inline constexpr std::uint32_t mncwf_event_current_valid = 1u << 1u;
inline constexpr std::uint32_t mncwf_event_end_valid = 1u << 2u;
inline constexpr std::uint32_t mncwf_event_trigger_valid = 1u << 3u;
inline constexpr std::uint32_t mncwf_event_tai_valid = 1u << 4u;
inline constexpr std::uint32_t mncwf_event_utc_valid = 1u << 5u;
inline constexpr std::uint32_t mncwf_event_settings_snapshot_valid = 1u << 6u;
inline constexpr std::uint32_t mncwf_event_contaminated = 1u << 7u;
inline constexpr std::uint32_t mncwf_event_discontinuous = 1u << 8u;
inline constexpr std::uint32_t mncwf_event_known_flags =
	mncwf_event_start_valid | mncwf_event_current_valid |
	mncwf_event_end_valid | mncwf_event_trigger_valid |
	mncwf_event_tai_valid | mncwf_event_utc_valid |
	mncwf_event_settings_snapshot_valid | mncwf_event_contaminated |
	mncwf_event_discontinuous;

inline constexpr std::uint32_t mncwf_quality_gap = 1u << 0u;
inline constexpr std::uint32_t mncwf_quality_saturated = 1u << 1u;
inline constexpr std::uint32_t mncwf_quality_clipped = 1u << 2u;
inline constexpr std::uint32_t mncwf_quality_transport_loss = 1u << 3u;
inline constexpr std::uint32_t mncwf_quality_timing_uncertain = 1u << 4u;
inline constexpr std::uint32_t mncwf_quality_calibration_invalid = 1u << 5u;
inline constexpr std::uint32_t mncwf_quality_rate_change = 1u << 6u;
inline constexpr std::uint32_t mncwf_quality_known_flags =
	mncwf_quality_gap | mncwf_quality_saturated | mncwf_quality_clipped |
	mncwf_quality_transport_loss | mncwf_quality_timing_uncertain |
	mncwf_quality_calibration_invalid | mncwf_quality_rate_change;

enum class MncwfLineageRelation : std::uint16_t {
	parent = 1,
	previous_continuation = 2,
	next_continuation = 3,
	event = 4,
	virtual_slice = 5,
};

using MncwfUuid = std::array<std::byte, 16>;
using MncwfSha256 = std::array<std::byte, 32>;

struct MncwfV4Header {
	std::uint32_t section_count = 0;
	std::uint64_t directory_offset = 0;
	std::uint64_t directory_bytes = 0;
	std::uint64_t file_bytes = 0;
	std::uint32_t flags = 0;
};

struct MncwfV4SectionInfo {
	std::uint32_t type = 0;
	std::uint16_t version = 0;
	std::uint16_t flags = 0;
	std::uint64_t offset = 0;
	std::uint64_t stored_bytes = 0;
	std::uint64_t logical_bytes = 0;
	std::uint64_t item_count = 0;
	std::uint32_t item_bytes = 0;
	std::uint32_t crc32c = 0;
	std::span<const std::byte> data{};
};

struct MncwfV4CaptureMetadata {
	MncwfUuid capture_uuid{};
	MncwfUuid device_uuid{};
	MncwfSha256 configuration_sha256{};
	MncwfSha256 sensor_profile_sha256{};
	std::uint64_t created_tai_nanoseconds = 0;
	std::uint64_t created_utc_nanoseconds = 0;
	std::int64_t nominal_voltage_numerator = 0;
	std::uint64_t nominal_voltage_denominator = 1;
	std::uint64_t nominal_frequency_numerator = 0;
	std::uint64_t nominal_frequency_denominator = 1;
	MncwfTopology topology = MncwfTopology::unknown;
	MncwfCalibrationStatus calibration_status =
		MncwfCalibrationStatus::unknown;
	std::uint32_t flags = 0;
	std::string station_name;
	std::string site_name;
	std::string circuit_name;
	std::string product_name;
	std::string device_model;
	std::string firmware_version;
	std::string software_build_id;
	std::string sensor_profile_id;
	std::string configuration_id;
	std::string calibration_id;
	std::string device_serial;
	std::string comments;
};

struct MncwfV4TimebaseSegment {
	std::uint64_t first_frame = 0;
	std::uint64_t frame_count = 0;
	std::uint64_t first_sequence = 0;
	std::uint64_t sequence_step = 0;
	std::uint64_t acquisition_rate_numerator = 0;
	std::uint64_t acquisition_rate_denominator = 1;
	std::uint64_t persisted_rate_numerator = 0;
	std::uint64_t persisted_rate_denominator = 1;
	std::uint64_t correlation_sequence = 0;
	std::uint64_t correlation_pl_tick = 0;
	std::uint64_t correlation_tai_nanoseconds = 0;
	std::uint64_t correlation_utc_nanoseconds = 0;
	std::uint64_t uncertainty_nanoseconds = 0;
	std::uint32_t decimation_divisor = 1;
	MncwfDecimationMethod decimation_method = MncwfDecimationMethod::none;
	MncwfClockSource clock_source = MncwfClockSource::unknown;
	MncwfTimeQuality time_quality = MncwfTimeQuality::unknown;
	std::uint16_t flags = 0;
	std::int32_t utc_offset_seconds = 0;
	/** Exact acquisition frames represented by this stored-frame segment. */
	std::uint64_t source_frame_count = 0;
};

struct MncwfV4ChannelDefinition {
	MncwfUuid stable_id{};
	std::uint32_t source_channel = 0;
	std::uint32_t flags = 0;
	MncwfPhase phase = MncwfPhase::none;
	MncwfQuantity quantity = MncwfQuantity::unknown;
	MncwfSiUnit si_unit = MncwfSiUnit::dimensionless;
	MncwfSampleEncoding sample_encoding =
		MncwfSampleEncoding::signed_integer_little_endian;
	std::uint16_t storage_bits = 0;
	std::uint16_t valid_bits = 0;
	std::int16_t display_exponent10 = 0;
	std::int64_t gain_numerator = 0;
	std::uint64_t gain_denominator = 1;
	std::int64_t offset_numerator = 0;
	std::uint64_t offset_denominator = 1;
	std::uint64_t primary_secondary_ratio_numerator = 1;
	std::uint64_t primary_secondary_ratio_denominator = 1;
	std::int64_t nominal_numerator = 0;
	std::uint64_t nominal_denominator = 1;
	std::int64_t range_minimum_numerator = 0;
	std::uint64_t range_minimum_denominator = 1;
	std::int64_t range_maximum_numerator = 0;
	std::uint64_t range_maximum_denominator = 1;
	std::uint64_t resolution_numerator = 0;
	std::uint64_t resolution_denominator = 1;
	std::int64_t clipping_low = 0;
	std::int64_t clipping_high = 0;
	std::string name;
	std::string unit_symbol;
	std::string description;
};

struct MncwfV4EventDescriptor {
	MncwfUuid event_uuid{};
	MncwfEventTaxonomy taxonomy = MncwfEventTaxonomy::unknown;
	std::uint16_t event_type = 0;
	MncwfEventLifecycle lifecycle = MncwfEventLifecycle::start;
	MncwfTimeQuality time_quality = MncwfTimeQuality::unknown;
	std::uint32_t flags = 0;
	std::uint32_t phase_mask = 0;
	MncwfQuantity quantity = MncwfQuantity::unknown;
	MncwfSiUnit si_unit = MncwfSiUnit::dimensionless;
	std::uint16_t trigger_source = 0;
	std::uint32_t configuration_generation = 0;
	std::uint32_t severity = 0;
	std::uint64_t start_sequence = 0;
	std::uint64_t current_sequence = 0;
	std::uint64_t end_sequence = 0;
	std::uint64_t trigger_sequence = 0;
	std::uint64_t start_tai_nanoseconds = 0;
	std::uint64_t current_tai_nanoseconds = 0;
	std::uint64_t end_tai_nanoseconds = 0;
	std::uint64_t trigger_tai_nanoseconds = 0;
	std::uint64_t start_utc_nanoseconds = 0;
	std::uint64_t current_utc_nanoseconds = 0;
	std::uint64_t end_utc_nanoseconds = 0;
	std::uint64_t trigger_utc_nanoseconds = 0;
	std::uint64_t uncertainty_nanoseconds = 0;
	std::int64_t reference_micro_units = 0;
	std::int64_t threshold_micro_units = 0;
	std::int64_t hysteresis_micro_units = 0;
	std::array<std::int64_t, 3> extrema_micro_units{};
	std::uint64_t duration_samples = 0;
	std::uint64_t update_count = 0;
	std::uint32_t status = 0;
	std::string taxonomy_name;
	std::string label;
	std::string settings_snapshot_json;
};

struct MncwfV4QualityInterval {
	std::uint64_t first_frame = 0;
	std::uint64_t frame_count = 0;
	std::uint64_t first_sequence = 0;
	std::uint64_t last_sequence = 0;
	std::uint64_t channel_mask = 0;
	std::uint32_t flags = 0;
	std::uint16_t severity = 0;
	std::uint16_t source = 0;
	std::uint32_t detail_code = 0;
};

struct MncwfV4LineageEntry {
	MncwfLineageRelation relation = MncwfLineageRelation::parent;
	std::uint16_t flags = 0;
	MncwfUuid related_capture_uuid{};
	MncwfUuid related_event_uuid{};
	std::uint64_t first_sequence = 0;
	std::uint64_t last_sequence = 0;
	std::uint32_t part_index = 0;
	std::uint32_t part_count = 0;
};

/** Reflected CRC-32C (Castagnoli), initial/final XOR 0xFFFFFFFF. */
[[nodiscard]] std::uint32_t mncwf_crc32c(
	std::span<const std::byte> bytes) noexcept;

/** SHA-256 helper used to freeze capture-time configuration authorities. */
[[nodiscard]] MncwfSha256 mncwf_sha256(
	std::span<const std::byte> bytes);
[[nodiscard]] inline MncwfSha256 mncwf_sha256(std::string_view text)
{
	return mncwf_sha256(std::as_bytes(std::span{text.data(), text.size()}));
}

/** Generate an RFC-4122 variant/version-4 UUID from the kernel RNG. */
[[nodiscard]] MncwfUuid mncwf_random_uuid();

/** Canonical lower-case RFC-4122 text form (8-4-4-4-12 hexadecimal). */
[[nodiscard]] std::string mncwf_uuid_string(const MncwfUuid &uuid);
/** Parse only the canonical RFC-4122 text form. */
[[nodiscard]] std::optional<MncwfUuid> mncwf_uuid_from_string(
	std::string_view text) noexcept;
/** True when every UUID byte is zero (the on-wire unavailable sentinel). */
[[nodiscard]] bool mncwf_uuid_is_zero(const MncwfUuid &uuid) noexcept;
/** Deterministic version-5 UUID for one stable R5C1 event identity. */
[[nodiscard]] MncwfUuid mncwf_stable_event_uuid(
	std::uint64_t session, std::uint64_t counter);

/**
 * Complete typed input to the deterministic version-4 encoder.
 *
 * sample_data contains exactly sample_frame_count interleaved frames of
 * sample_frame_bytes bytes. The encoder validates its own result with
 * MncwfV4Reader before returning it, so a writer bug cannot publish a file
 * that the product reader rejects.
 */
struct MncwfV4Document {
	MncwfV4CaptureMetadata capture_metadata{};
	std::vector<MncwfV4TimebaseSegment> timebase_segments;
	std::vector<MncwfV4ChannelDefinition> channels;
	std::vector<MncwfV4EventDescriptor> events;
	std::vector<MncwfV4QualityInterval> quality_intervals;
	std::vector<MncwfV4LineageEntry> lineage;
	std::uint64_t sample_frame_count = 0;
	std::uint32_t sample_frame_bytes = 0;
	std::vector<std::byte> sample_data;
};

/** Encode all seven mandatory sections with deterministic ordering and CRCs. */
[[nodiscard]] std::vector<std::byte>
encode_mncwf_v4(const MncwfV4Document &document);

/**
 * Defensive, read-only MNCWF v4 view.
 *
 * The caller owns the byte storage and must keep it alive for the lifetime of
 * the reader. Construction validates the complete file before any typed
 * metadata or sample span is exposed.
 */
class MncwfV4Reader {
public:
	explicit MncwfV4Reader(std::span<const std::byte> bytes);

	[[nodiscard]] const MncwfV4Header &header() const noexcept
	{
		return header_;
	}
	[[nodiscard]] const std::vector<MncwfV4SectionInfo> &sections() const noexcept
	{
		return sections_;
	}
	[[nodiscard]] const MncwfV4CaptureMetadata &capture_metadata() const noexcept
	{
		return capture_metadata_;
	}
	[[nodiscard]] const std::vector<MncwfV4TimebaseSegment> &
	timebase_segments() const noexcept
	{
		return timebase_segments_;
	}
	[[nodiscard]] const std::vector<MncwfV4ChannelDefinition> &
	channels() const noexcept
	{
		return channels_;
	}
	[[nodiscard]] const std::vector<MncwfV4EventDescriptor> &events() const noexcept
	{
		return events_;
	}
	[[nodiscard]] const std::vector<MncwfV4QualityInterval> &
	quality_intervals() const noexcept
	{
		return quality_intervals_;
	}
	[[nodiscard]] const std::vector<MncwfV4LineageEntry> &lineage() const noexcept
	{
		return lineage_;
	}
	[[nodiscard]] std::span<const std::byte> sample_data() const noexcept
	{
		return sample_data_;
	}
	[[nodiscard]] std::uint64_t sample_frame_count() const noexcept
	{
		return sample_frame_count_;
	}
	[[nodiscard]] std::uint32_t sample_frame_bytes() const noexcept
	{
		return sample_frame_bytes_;
	}
	[[nodiscard]] std::span<const std::byte> sample_frame(
		std::uint64_t index) const;

private:
	std::span<const std::byte> bytes_{};
	MncwfV4Header header_{};
	std::vector<MncwfV4SectionInfo> sections_;
	MncwfV4CaptureMetadata capture_metadata_{};
	std::vector<MncwfV4TimebaseSegment> timebase_segments_;
	std::vector<MncwfV4ChannelDefinition> channels_;
	std::vector<MncwfV4EventDescriptor> events_;
	std::vector<MncwfV4QualityInterval> quality_intervals_;
	std::vector<MncwfV4LineageEntry> lineage_;
	std::span<const std::byte> sample_data_{};
	std::uint64_t sample_frame_count_ = 0;
	std::uint32_t sample_frame_bytes_ = 0;
};

/**
 * Read-only, non-owning event subcapture encoded as a complete MNCWF v4 file.
 *
 * Metadata, directory entries, CRC32C values, and zero padding are owned by
 * this object. The sample region remains a span into the validated parent
 * reader, so the parent's byte storage must outlive the virtual file. read()
 * supports bounded sequential or random-access delivery without materializing
 * or persisting another waveform-sized buffer.
 */
class MncwfV4VirtualFile {
public:
	[[nodiscard]] std::uint64_t size() const noexcept { return file_bytes_; }
	[[nodiscard]] const MncwfUuid &capture_uuid() const noexcept
	{
		return capture_uuid_;
	}
	[[nodiscard]] std::uint64_t first_sequence() const noexcept
	{
		return first_sequence_;
	}
	[[nodiscard]] std::uint64_t last_sequence() const noexcept
	{
		return last_sequence_;
	}
	/** Copy at most destination.size() bytes beginning at file offset. */
	[[nodiscard]] std::size_t read(std::uint64_t offset,
		std::span<std::byte> destination) const noexcept;

private:
	friend MncwfV4VirtualFile make_mncwf_v4_event_slice(
		const MncwfV4Reader &, const MncwfUuid &);
	std::vector<std::byte> prefix_;
	std::span<const std::byte> sample_data_{};
	std::uint64_t file_bytes_ = 0;
	std::uint8_t trailing_padding_bytes_ = 0;
	MncwfUuid capture_uuid_{};
	std::uint64_t first_sequence_ = 0;
	std::uint64_t last_sequence_ = 0;
};

/** Build a deterministic virtual subcapture around one event UUID. */
[[nodiscard]] MncwfV4VirtualFile make_mncwf_v4_event_slice(
	const MncwfV4Reader &reader, const MncwfUuid &event_uuid);

/**
 * Missing MNCWF source fields for later, full-fidelity event exports.
 *
 * This does not implement or advertise either converter. Empty vectors mean
 * a converter can work from this file alone without consulting live settings
 * or an external device database.
 */
struct MncwfV4ConversionReadiness {
	std::vector<std::string> comtrade_missing;
	std::vector<std::string> pqdif_missing;

	[[nodiscard]] bool comtrade_ready() const noexcept
	{
		return comtrade_missing.empty();
	}
	[[nodiscard]] bool pqdif_ready() const noexcept
	{
		return pqdif_missing.empty();
	}
};

[[nodiscard]] MncwfV4ConversionReadiness
assess_mncwf_v4_conversion_readiness(const MncwfV4Reader &reader);

} // namespace msap1
