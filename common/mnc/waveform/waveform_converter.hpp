#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace mnc::waveform {

using Uuid = std::array<std::byte, 16>;
using Sha256 = std::array<std::byte, 32>;

enum class ExportFormat : std::uint8_t {
	comtrade = 1,
	pqdif = 2,
	comtrade_zip = 3,
};
enum class ExportScope : std::uint8_t { capture = 1, event = 2 };

enum class Phase : std::uint16_t {
	none = 0, a = 1, b = 2, c = 3, neutral = 4, ab = 5, bc = 6, ca = 7,
};
enum class Quantity : std::uint16_t {
	unknown = 0, current = 1, voltage = 2, status = 3,
	frequency = 4, ratio = 5,
};
enum class SiUnit : std::uint16_t {
	dimensionless = 0, ampere = 1, volt = 2, hertz = 3,
};
enum class Topology : std::uint32_t { unknown = 0, wye = 1, delta = 2 };
enum class CalibrationStatus : std::uint32_t {
	unknown = 0, valid = 1, expired = 2, invalid = 3,
};
enum class DecimationMethod : std::uint16_t {
	none = 0, boxcar_mean_toward_zero = 1,
};
enum class ClockSource : std::uint16_t {
	unknown = 0, system = 1, ptp = 2, gnss = 3, manual = 4,
};
enum class TimeQuality : std::uint16_t {
	unknown = 0, unlocked = 1, holdover = 2, locked = 3,
};
enum class EventTaxonomy : std::uint16_t {
	unknown = 0, iec_61000_4_30 = 1, product_alarm = 2,
};
enum class EventLifecycle : std::uint16_t {
	start = 1, update = 2, end = 3, abort = 4, complete = 5,
};
enum class LineageRelation : std::uint16_t {
	parent = 1, previous_continuation = 2, next_continuation = 3,
	event = 4, virtual_slice = 5,
};

inline constexpr std::uint16_t time_utc_offset_known = 1u << 0u;
inline constexpr std::uint16_t time_positive_leap_pending = 1u << 1u;
inline constexpr std::uint16_t time_negative_leap_pending = 1u << 2u;
inline constexpr std::uint16_t time_rate_change_before = 1u << 3u;
inline constexpr std::uint16_t time_sequence_gap_before = 1u << 4u;

inline constexpr std::uint32_t channel_enabled = 1u << 0u;
inline constexpr std::uint32_t channel_transform_valid = 1u << 1u;
inline constexpr std::uint32_t channel_ratio_valid = 1u << 2u;
inline constexpr std::uint32_t channel_nominal_valid = 1u << 3u;
inline constexpr std::uint32_t channel_range_valid = 1u << 4u;
inline constexpr std::uint32_t channel_resolution_valid = 1u << 5u;
inline constexpr std::uint32_t channel_clipping_valid = 1u << 6u;
inline constexpr std::uint32_t channel_calibration_valid = 1u << 7u;

inline constexpr std::uint32_t event_start_valid = 1u << 0u;
inline constexpr std::uint32_t event_current_valid = 1u << 1u;
inline constexpr std::uint32_t event_end_valid = 1u << 2u;
inline constexpr std::uint32_t event_trigger_valid = 1u << 3u;
inline constexpr std::uint32_t event_tai_valid = 1u << 4u;
inline constexpr std::uint32_t event_utc_valid = 1u << 5u;
inline constexpr std::uint32_t event_settings_snapshot_valid = 1u << 6u;
inline constexpr std::uint32_t event_contaminated = 1u << 7u;
inline constexpr std::uint32_t event_discontinuous = 1u << 8u;

inline constexpr std::uint32_t quality_gap = 1u << 0u;
inline constexpr std::uint32_t quality_saturated = 1u << 1u;
inline constexpr std::uint32_t quality_clipped = 1u << 2u;
inline constexpr std::uint32_t quality_transport_loss = 1u << 3u;
inline constexpr std::uint32_t quality_timing_uncertain = 1u << 4u;
inline constexpr std::uint32_t quality_calibration_invalid = 1u << 5u;
inline constexpr std::uint32_t quality_rate_change = 1u << 6u;

struct SignedRational {
	std::int64_t numerator = 0;
	std::uint64_t denominator = 1;
};

struct UnsignedRational {
	std::uint64_t numerator = 0;
	std::uint64_t denominator = 1;
};

struct CaptureMetadata {
	Uuid capture_uuid{};
	Uuid source_capture_uuid{};
	Uuid device_uuid{};
	Sha256 configuration_sha256{};
	Sha256 sensor_profile_sha256{};
	std::uint64_t created_tai_nanoseconds = 0;
	std::uint64_t created_utc_nanoseconds = 0;
	SignedRational nominal_voltage{};
	UnsignedRational nominal_frequency{};
	Topology topology = Topology::unknown;
	CalibrationStatus calibration_status = CalibrationStatus::unknown;
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

struct TimebaseSegment {
	std::uint64_t first_frame = 0;
	std::uint64_t frame_count = 0;
	std::uint64_t first_sequence = 0;
	std::uint64_t sequence_step = 0;
	UnsignedRational acquisition_rate{};
	UnsignedRational persisted_rate{};
	std::uint64_t correlation_sequence = 0;
	std::uint64_t correlation_pl_tick = 0;
	std::uint64_t correlation_tai_nanoseconds = 0;
	std::uint64_t correlation_utc_nanoseconds = 0;
	std::uint64_t uncertainty_nanoseconds = 0;
	std::uint32_t decimation_divisor = 1;
	DecimationMethod decimation_method = DecimationMethod::none;
	ClockSource clock_source = ClockSource::unknown;
	TimeQuality time_quality = TimeQuality::unknown;
	std::uint16_t flags = 0;
	std::int32_t utc_offset_seconds = 0;
	std::uint64_t source_frame_count = 0;
};

struct ChannelDefinition {
	Uuid stable_id{};
	std::uint32_t source_channel = 0;
	std::uint32_t flags = 0;
	Phase phase = Phase::none;
	Quantity quantity = Quantity::unknown;
	SiUnit si_unit = SiUnit::dimensionless;
	std::uint16_t storage_bits = 0;
	std::uint16_t valid_bits = 0;
	std::int16_t display_exponent10 = 0;
	SignedRational gain{};
	SignedRational offset{};
	UnsignedRational primary_secondary_ratio{1, 1};
	SignedRational nominal{};
	SignedRational range_minimum{};
	SignedRational range_maximum{};
	UnsignedRational resolution{};
	std::int64_t clipping_low = 0;
	std::int64_t clipping_high = 0;
	std::string name;
	std::string unit_symbol;
	std::string description;
};

struct EventDescriptor {
	Uuid event_uuid{};
	EventTaxonomy taxonomy = EventTaxonomy::unknown;
	std::uint16_t event_type = 0;
	EventLifecycle lifecycle = EventLifecycle::start;
	TimeQuality time_quality = TimeQuality::unknown;
	std::uint32_t flags = 0;
	std::uint32_t phase_mask = 0;
	Quantity quantity = Quantity::unknown;
	SiUnit si_unit = SiUnit::dimensionless;
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

struct QualityInterval {
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

struct LineageEntry {
	LineageRelation relation = LineageRelation::parent;
	std::uint16_t flags = 0;
	Uuid related_capture_uuid{};
	Uuid related_event_uuid{};
	std::uint64_t first_sequence = 0;
	std::uint64_t last_sequence = 0;
	std::uint32_t part_index = 0;
	std::uint32_t part_count = 0;
};

struct WaveformMetadata {
	CaptureMetadata capture;
	std::vector<TimebaseSegment> timebase_segments;
	std::vector<ChannelDefinition> channels;
	std::vector<EventDescriptor> events;
	std::vector<QualityInterval> quality_intervals;
	std::vector<LineageEntry> lineage;
	std::optional<Uuid> selected_event_uuid;
	std::uint64_t source_first_frame = 0;
	std::uint64_t first_sequence = 0;
	std::uint64_t last_sequence = 0;
};

class WaveformSource {
public:
	virtual ~WaveformSource() = default;
	[[nodiscard]] virtual const WaveformMetadata &metadata() const noexcept = 0;
	[[nodiscard]] virtual std::uint64_t frame_count() const noexcept = 0;
	[[nodiscard]] virtual std::size_t channel_count() const noexcept = 0;

	/**
	 * Read up to @p frame_capacity interleaved signed frames. The destination
	 * must hold frame_capacity * channel_count() values. A successful read
	 * returns at least one frame unless first_frame == frame_count().
	 */
	[[nodiscard]] virtual std::size_t read_frames(std::uint64_t first_frame,
		std::size_t frame_capacity, std::span<std::int64_t> destination) const = 0;
};

class OutputSink {
public:
	virtual ~OutputSink() = default;
	virtual void write(std::span<const std::byte> bytes) = 0;
	[[nodiscard]] virtual std::uint64_t bytes_written() const noexcept = 0;
	[[nodiscard]] virtual std::uint64_t byte_limit() const noexcept = 0;
};

class VectorOutputSink final : public OutputSink {
public:
	explicit VectorOutputSink(std::uint64_t limit = 1024ull * 1024ull * 1024ull)
		: limit_(limit) {}
	void write(std::span<const std::byte> bytes) override;
	[[nodiscard]] std::uint64_t bytes_written() const noexcept override;
	[[nodiscard]] std::uint64_t byte_limit() const noexcept override;
	[[nodiscard]] const std::vector<std::byte> &bytes() const noexcept
	{
		return bytes_;
	}

private:
	std::uint64_t limit_;
	std::vector<std::byte> bytes_;
};

enum class ConversionErrorCode : std::uint16_t {
	invalid_options = 1,
	unsupported_format,
	unsupported_source_version,
	source_invalid,
	source_incomplete,
	source_not_ready,
	source_event_not_found,
	source_discontinuity_unsupported,
	sample_out_of_range,
	timestamp_out_of_range,
	output_too_large,
	output_write_failed,
	conversion_cancelled,
	validation_failed,
	internal_error,
};

[[nodiscard]] std::string_view conversion_error_code_name(
	ConversionErrorCode code) noexcept;

class ConversionError final : public std::runtime_error {
public:
	ConversionError(ConversionErrorCode code, std::string message,
		std::vector<std::string> missing_fields = {});
	[[nodiscard]] ConversionErrorCode code() const noexcept { return code_; }
	[[nodiscard]] const std::vector<std::string> &missing_fields() const noexcept
	{
		return missing_fields_;
	}

private:
	ConversionErrorCode code_;
	std::vector<std::string> missing_fields_;
};

struct ConversionOptions {
	ExportFormat format = ExportFormat::comtrade;
	ExportScope scope = ExportScope::capture;
	std::optional<Uuid> selected_event_uuid;
	std::uint64_t maximum_output_bytes = 1024ull * 1024ull * 1024ull;
	std::size_t frame_batch_size = 4096;
	bool pqdif_record_compression = true;
	/** Basename used for members of container formats; never a path. */
	std::string output_stem{"waveform"};
};

struct ConversionProgress {
	std::uint64_t processed_frames = 0;
	std::uint64_t total_frames = 0;
};
using ProgressCallback = std::function<void(const ConversionProgress &)>;

struct ConversionSummary {
	ExportFormat format = ExportFormat::comtrade;
	std::string profile;
	std::uint64_t frames = 0;
	std::uint64_t bytes = 0;
	std::string extension;
	std::string media_type;
};

class WaveformConverter {
public:
	virtual ~WaveformConverter() = default;
	[[nodiscard]] virtual ConversionSummary convert(const WaveformSource &source,
		OutputSink &sink, const ConversionOptions &options,
		std::stop_token stop_token = {},
		ProgressCallback progress = {}) const = 0;
};

[[nodiscard]] std::unique_ptr<WaveformConverter> make_converter(
	ExportFormat format);
[[nodiscard]] std::string_view export_format_name(ExportFormat value) noexcept;
[[nodiscard]] std::optional<ExportFormat> export_format_from_name(
	std::string_view value) noexcept;
[[nodiscard]] std::string uuid_string(const Uuid &value);
[[nodiscard]] bool uuid_is_zero(const Uuid &value) noexcept;
/** RFC-4122 deterministic name UUID (version 5). */
[[nodiscard]] Uuid uuid_v5(const Uuid &name_space, std::string_view name);

/** Shared preflight enforced by every destination converter. */
void validate_conversion_source(const WaveformSource &source,
	const ConversionOptions &options);

} // namespace mnc::waveform
