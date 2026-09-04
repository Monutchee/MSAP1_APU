#include "mnc/waveform/waveform_converter.hpp"

#include "mnc/waveform/comtrade_converter.hpp"
#include "mnc/waveform/comtrade_zip_converter.hpp"
#include "mnc/waveform/pqdif_converter.hpp"

#include <boost/uuid/name_generator_sha1.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/multiprecision/cpp_int.hpp>

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>

namespace mnc::waveform {

void VectorOutputSink::write(std::span<const std::byte> bytes)
{
	if (bytes.size() > limit_ - bytes_.size())
		throw ConversionError(ConversionErrorCode::output_too_large,
			"waveform export exceeds its output byte limit");
	bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
}

std::uint64_t VectorOutputSink::bytes_written() const noexcept
{
	return bytes_.size();
}

std::uint64_t VectorOutputSink::byte_limit() const noexcept { return limit_; }

std::string_view conversion_error_code_name(ConversionErrorCode code) noexcept
{
	switch (code) {
	case ConversionErrorCode::invalid_options: return "invalid_options";
	case ConversionErrorCode::unsupported_format: return "unsupported_format";
	case ConversionErrorCode::unsupported_source_version:
		return "unsupported_source_version";
	case ConversionErrorCode::source_invalid: return "source_invalid";
	case ConversionErrorCode::source_incomplete: return "source_incomplete";
	case ConversionErrorCode::source_not_ready: return "source_not_ready";
	case ConversionErrorCode::source_event_not_found:
		return "source_event_not_found";
	case ConversionErrorCode::source_discontinuity_unsupported:
		return "source_discontinuity_unsupported";
	case ConversionErrorCode::sample_out_of_range: return "sample_out_of_range";
	case ConversionErrorCode::timestamp_out_of_range:
		return "timestamp_out_of_range";
	case ConversionErrorCode::output_too_large: return "output_too_large";
	case ConversionErrorCode::output_write_failed: return "output_write_failed";
	case ConversionErrorCode::conversion_cancelled:
		return "conversion_cancelled";
	case ConversionErrorCode::validation_failed: return "validation_failed";
	case ConversionErrorCode::internal_error: return "internal_error";
	}
	return "internal_error";
}

ConversionError::ConversionError(ConversionErrorCode code, std::string message,
	std::vector<std::string> missing_fields)
	: std::runtime_error(std::move(message)), code_(code),
	  missing_fields_(std::move(missing_fields))
{
}

std::unique_ptr<WaveformConverter> make_converter(ExportFormat format)
{
	switch (format) {
	case ExportFormat::comtrade: return std::make_unique<ComtradeConverter>();
	case ExportFormat::pqdif: return std::make_unique<PqdifConverter>();
	case ExportFormat::comtrade_zip:
		return std::make_unique<ComtradeZipConverter>();
	}
	throw ConversionError(ConversionErrorCode::unsupported_format,
		"unsupported waveform export format");
}

std::string_view export_format_name(ExportFormat value) noexcept
{
	switch (value) {
	case ExportFormat::comtrade: return "comtrade";
	case ExportFormat::pqdif: return "pqdif";
	case ExportFormat::comtrade_zip: return "comtrade-zip";
	}
	return "unknown";
}

std::optional<ExportFormat> export_format_from_name(std::string_view value) noexcept
{
	if (value == "comtrade") return ExportFormat::comtrade;
	if (value == "pqdif") return ExportFormat::pqdif;
	if (value == "comtrade-zip") return ExportFormat::comtrade_zip;
	return std::nullopt;
}

std::string uuid_string(const Uuid &value)
{
	std::ostringstream output;
	output << std::hex << std::setfill('0');
	for (std::size_t index = 0; index < value.size(); ++index) {
		if (index == 4u || index == 6u || index == 8u || index == 10u)
			output << '-';
		output << std::setw(2)
		       << static_cast<unsigned>(std::to_integer<std::uint8_t>(value[index]));
	}
	return output.str();
}

bool uuid_is_zero(const Uuid &value) noexcept
{
	return std::ranges::all_of(value,
		[](std::byte byte) { return byte == std::byte{0}; });
}

Uuid uuid_v5(const Uuid &name_space, std::string_view name)
{
	boost::uuids::uuid boost_namespace{};
	std::transform(name_space.begin(), name_space.end(), boost_namespace.begin(),
		[](std::byte byte) { return std::to_integer<std::uint8_t>(byte); });
	const auto generated = boost::uuids::name_generator_sha1(boost_namespace)(
		std::string(name));
	Uuid result{};
	std::transform(generated.begin(), generated.end(), result.begin(),
		[](std::uint8_t byte) { return static_cast<std::byte>(byte); });
	return result;
}

void validate_conversion_source(const WaveformSource &source,
	const ConversionOptions &options)
{
	using boost::multiprecision::cpp_int;

	if (options.maximum_output_bytes == 0 || options.frame_batch_size == 0)
		throw ConversionError(ConversionErrorCode::invalid_options,
			"conversion output and frame-batch limits must be nonzero");
	if (source.frame_count() == 0 || source.channel_count() == 0 ||
	    source.channel_count() != source.metadata().channels.size())
		throw ConversionError(ConversionErrorCode::source_invalid,
			"waveform source has invalid sample geometry");
	const auto &metadata = source.metadata();
	if (metadata.timebase_segments.empty())
		throw ConversionError(ConversionErrorCode::source_invalid,
			"waveform source has no timebase");
	std::uint64_t expected_frame = 0;
	std::optional<std::uint64_t> expected_sequence;
	for (const auto &segment : metadata.timebase_segments) {
		if (segment.first_frame != expected_frame || segment.frame_count == 0 ||
		    segment.sequence_step == 0 || segment.source_frame_count == 0 ||
		    segment.acquisition_rate.numerator == 0 ||
		    segment.acquisition_rate.denominator == 0 ||
		    segment.persisted_rate.numerator == 0 ||
		    segment.persisted_rate.denominator == 0 ||
		    segment.decimation_divisor == 0)
			throw ConversionError(ConversionErrorCode::source_invalid,
				"waveform source has invalid timebase geometry");
		if ((segment.flags & time_sequence_gap_before) != 0u)
			throw ConversionError(
				ConversionErrorCode::source_discontinuity_unsupported,
				"selected waveform interval contains a timebase gap or discontinuity");
		if (expected_sequence && segment.first_sequence != *expected_sequence)
			throw ConversionError(
				ConversionErrorCode::source_discontinuity_unsupported,
				"selected waveform interval contains a sequence discontinuity");
		if (segment.decimation_method == DecimationMethod::none) {
			if (segment.decimation_divisor != 1u || segment.sequence_step != 1u ||
			    segment.source_frame_count != segment.frame_count)
				throw ConversionError(ConversionErrorCode::source_invalid,
					"non-decimated waveform timebase geometry is invalid");
		} else if (segment.decimation_method ==
			DecimationMethod::boxcar_mean_toward_zero) {
			const cpp_int minimum = cpp_int(segment.frame_count - 1u) *
				segment.decimation_divisor + 1u;
			const cpp_int maximum = cpp_int(segment.frame_count) *
				segment.decimation_divisor;
			if (segment.sequence_step != segment.decimation_divisor ||
			    cpp_int(segment.source_frame_count) < minimum ||
			    cpp_int(segment.source_frame_count) > maximum)
				throw ConversionError(ConversionErrorCode::source_invalid,
					"boxcar waveform timebase geometry is invalid");
		} else {
			throw ConversionError(ConversionErrorCode::source_invalid,
				"waveform timebase uses an unknown decimation method");
		}
		const cpp_int persisted_left =
			cpp_int(segment.persisted_rate.numerator) *
			segment.acquisition_rate.denominator * segment.decimation_divisor;
		const cpp_int acquisition_right =
			cpp_int(segment.acquisition_rate.numerator) *
			segment.persisted_rate.denominator;
		if (persisted_left != acquisition_right)
			throw ConversionError(ConversionErrorCode::source_invalid,
				"persisted waveform rate does not match acquisition rate and decimation");
		const cpp_int sequence_end = cpp_int(segment.first_sequence) +
			segment.source_frame_count;
		if (sequence_end > std::numeric_limits<std::uint64_t>::max())
			throw ConversionError(ConversionErrorCode::source_invalid,
				"waveform timebase sequence range overflows");
		expected_sequence = sequence_end.convert_to<std::uint64_t>();
		if (segment.frame_count >
		    std::numeric_limits<std::uint64_t>::max() - expected_frame)
			throw ConversionError(ConversionErrorCode::source_invalid,
				"waveform timebase frame count overflows");
		expected_frame += segment.frame_count;
	}
	if (expected_frame != source.frame_count())
		throw ConversionError(ConversionErrorCode::source_invalid,
			"waveform timebase does not cover every selected frame");
	if (metadata.first_sequence != metadata.timebase_segments.front().first_sequence ||
	    !expected_sequence || *expected_sequence == 0u ||
	    metadata.last_sequence != *expected_sequence - 1u ||
	    metadata.last_sequence < metadata.first_sequence)
		throw ConversionError(ConversionErrorCode::source_invalid,
			"waveform selection sequence bounds disagree with its timebase");
	for (const auto &interval : metadata.quality_intervals) {
		if (interval.frame_count == 0 || interval.first_frame > source.frame_count() ||
		    interval.frame_count > source.frame_count() - interval.first_frame)
			throw ConversionError(ConversionErrorCode::source_invalid,
				"waveform quality interval is outside the selected frames");
		if ((interval.flags & (quality_gap | quality_transport_loss)) != 0u)
			throw ConversionError(
				ConversionErrorCode::source_discontinuity_unsupported,
				"selected waveform interval contains a sequence gap or transport loss");
	}
	if (options.scope == ExportScope::event) {
		const auto requested = options.selected_event_uuid
			? options.selected_event_uuid : metadata.selected_event_uuid;
		if (!requested || uuid_is_zero(*requested))
			throw ConversionError(ConversionErrorCode::invalid_options,
				"event export requires a selected event UUID");
		if (std::ranges::none_of(metadata.events, [&](const auto &event) {
			return event.event_uuid == *requested;
		}))
			throw ConversionError(ConversionErrorCode::source_event_not_found,
				"selected event is not present in the waveform interval");
	}
}

} // namespace mnc::waveform
