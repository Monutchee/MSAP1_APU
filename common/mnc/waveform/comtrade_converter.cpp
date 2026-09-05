#include "mnc/waveform/comtrade_converter.hpp"

#include <boost/multiprecision/cpp_dec_float.hpp>
#include <boost/multiprecision/cpp_int.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <limits>
#include <optional>
#include <ranges>
#include <sstream>
#include <utility>

namespace mnc::waveform {
namespace {

using boost::multiprecision::cpp_dec_float_50;
using boost::multiprecision::cpp_int;

struct BigRational {
	cpp_int numerator{0};
	cpp_int denominator{1};
};

[[noreturn]] void cancelled()
{
	throw ConversionError(ConversionErrorCode::conversion_cancelled,
		"waveform conversion was cancelled");
}

void check_cancelled(std::stop_token token)
{
	if (token.stop_requested())
		cancelled();
}

std::string clean(std::string_view value, std::size_t maximum = 128)
{
	std::string result;
	result.reserve(std::min(value.size(), maximum));
	for (const unsigned char character : value) {
		if (result.size() == maximum)
			break;
		if (character == ',' || character == '\r' || character == '\n' ||
		    character < 0x20u || character == 0x7fu)
			result.push_back(' ');
		else if (character < 0x80u)
			result.push_back(static_cast<char>(character));
		else
			result.push_back('?');
	}
	return result;
}

std::string clean_line(std::string_view value, std::size_t maximum = 4096)
{
	std::string result;
	result.reserve(std::min(value.size(), maximum));
	for (const unsigned char character : value) {
		if (result.size() == maximum)
			break;
		if (character == '\r' || character == '\n' || character < 0x20u ||
		    character == 0x7fu)
			result.push_back(' ');
		else if (character < 0x80u)
			result.push_back(static_cast<char>(character));
		else
			result.push_back('?');
	}
	return result;
}

std::string decimal(std::int64_t numerator, std::uint64_t denominator)
{
	if (denominator == 0)
		throw ConversionError(ConversionErrorCode::source_invalid,
			"waveform metadata contains a zero rational denominator");
	cpp_dec_float_50 value(numerator);
	value /= cpp_dec_float_50(denominator);
	std::ostringstream output;
	output << std::setprecision(36) << std::scientific << value;
	return output.str();
}

std::string decimal(std::uint64_t numerator, std::uint64_t denominator)
{
	if (numerator > static_cast<std::uint64_t>(
		std::numeric_limits<std::int64_t>::max())) {
		if (denominator == 0)
			throw ConversionError(ConversionErrorCode::source_invalid,
				"waveform metadata contains a zero rational denominator");
		cpp_dec_float_50 value(numerator);
		value /= cpp_dec_float_50(denominator);
		std::ostringstream output;
		output << std::setprecision(36) << std::scientific << value;
		return output.str();
	}
	return decimal(static_cast<std::int64_t>(numerator), denominator);
}

std::string phase_name(Phase phase)
{
	switch (phase) {
	case Phase::a: return "A";
	case Phase::b: return "B";
	case Phase::c: return "C";
	case Phase::neutral: return "N";
	case Phase::ab: return "AB";
	case Phase::bc: return "BC";
	case Phase::ca: return "CA";
	case Phase::none: return {};
	}
	return {};
}

std::string unit_name(const ChannelDefinition &channel)
{
	if (!channel.unit_symbol.empty())
		return clean(channel.unit_symbol, 32);
	switch (channel.si_unit) {
	case SiUnit::ampere: return "A";
	case SiUnit::volt: return "V";
	case SiUnit::hertz: return "Hz";
	case SiUnit::dimensionless: return "none";
	}
	return "none";
}

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
			"sample frame has no timebase segment");
	return *found;
}

BigRational utc_for_sequence(const TimebaseSegment &segment,
	std::uint64_t sequence)
{
	if (segment.correlation_utc_nanoseconds == 0 ||
	    segment.acquisition_rate.numerator == 0 ||
	    segment.acquisition_rate.denominator == 0)
		throw ConversionError(ConversionErrorCode::source_not_ready,
			"waveform timebase lacks an exact UTC correlation");
	const cpp_int denominator = segment.acquisition_rate.numerator;
	cpp_int numerator = cpp_int(segment.correlation_utc_nanoseconds) * denominator;
	const cpp_int delta = sequence >= segment.correlation_sequence
		? cpp_int(sequence - segment.correlation_sequence)
		: -cpp_int(segment.correlation_sequence - sequence);
	numerator += delta * segment.acquisition_rate.denominator * 1'000'000'000ull;
	return {std::move(numerator), denominator};
}

BigRational utc_for_frame(const WaveformMetadata &metadata, std::uint64_t frame)
{
	const auto &segment = segment_for_frame(metadata, frame);
	const auto local = frame - segment.first_frame;
	const cpp_int sequence_big = cpp_int(segment.first_sequence) +
		cpp_int(local) * segment.sequence_step;
	if (sequence_big < 0 || sequence_big >
	    std::numeric_limits<std::uint64_t>::max())
		throw ConversionError(ConversionErrorCode::timestamp_out_of_range,
			"sample sequence is outside the supported range");
	return utc_for_sequence(segment, sequence_big.convert_to<std::uint64_t>());
}

std::pair<std::uint64_t, std::uint64_t> sequence_range_for_frame(
	const WaveformMetadata &metadata, std::uint64_t frame)
{
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
			"sample frame source-sequence range is invalid");
	return {first.convert_to<std::uint64_t>(),
		last.convert_to<std::uint64_t>()};
}

BigRational subtract(const BigRational &left, const BigRational &right)
{
	return {left.numerator * right.denominator -
			right.numerator * left.denominator,
		left.denominator * right.denominator};
}

std::uint64_t round_half_even(const BigRational &value)
{
	if (value.denominator <= 0 || value.numerator < 0)
		throw ConversionError(ConversionErrorCode::timestamp_out_of_range,
			"COMTRADE relative timestamp is negative or invalid");
	cpp_int quotient = value.numerator / value.denominator;
	const cpp_int remainder = value.numerator % value.denominator;
	const auto doubled = remainder * 2;
	if (doubled > value.denominator ||
	    (doubled == value.denominator && (quotient & 1) != 0))
		++quotient;
	if (quotient > std::numeric_limits<std::uint64_t>::max())
		throw ConversionError(ConversionErrorCode::timestamp_out_of_range,
			"COMTRADE timestamp exceeds the supported integer range");
	return quotient.convert_to<std::uint64_t>();
}

std::int64_t round_signed_half_even(const BigRational &value)
{
	if (value.denominator <= 0)
		throw ConversionError(ConversionErrorCode::timestamp_out_of_range,
			"invalid timestamp rational");
	const bool negative = value.numerator < 0;
	BigRational absolute{negative ? -value.numerator : value.numerator,
		value.denominator};
	const auto rounded = round_half_even(absolute);
	if (rounded > static_cast<std::uint64_t>(
		std::numeric_limits<std::int64_t>::max()))
		throw ConversionError(ConversionErrorCode::timestamp_out_of_range,
			"absolute timestamp exceeds the supported range");
	return negative ? -static_cast<std::int64_t>(rounded)
		: static_cast<std::int64_t>(rounded);
}

std::string timestamp_text(const BigRational &timestamp)
{
	const BigRational ns{timestamp.numerator,
		timestamp.denominator};
	const auto rounded_ns = round_signed_half_even(ns);
	const auto seconds = rounded_ns / 1'000'000'000ll;
	auto fraction = rounded_ns % 1'000'000'000ll;
	std::time_t time = static_cast<std::time_t>(seconds);
	if (fraction < 0) {
		--time;
		fraction += 1'000'000'000ll;
	}
	std::tm utc{};
	if (::gmtime_r(&time, &utc) == nullptr)
		throw ConversionError(ConversionErrorCode::timestamp_out_of_range,
			"UTC timestamp cannot be represented by the platform");
	char prefix[32]{};
	if (std::strftime(prefix, sizeof(prefix), "%d/%m/%Y,%H:%M:%S", &utc) == 0)
		throw ConversionError(ConversionErrorCode::timestamp_out_of_range,
			"UTC timestamp formatting failed");
	std::ostringstream output;
	output << prefix << '.' << std::setw(9) << std::setfill('0') << fraction;
	return output.str();
}

const EventDescriptor &trigger_event(const WaveformMetadata &metadata,
	const ConversionOptions &options)
{
	if (metadata.events.empty())
		throw ConversionError(ConversionErrorCode::source_not_ready,
			"COMTRADE export requires at least one captured event",
			{"event_descriptor"});
	if (options.scope == ExportScope::event) {
		const auto selected = options.selected_event_uuid
			? options.selected_event_uuid : metadata.selected_event_uuid;
		const auto found = std::ranges::find_if(metadata.events,
			[&](const auto &event) { return selected && event.event_uuid == *selected; });
		if (found == metadata.events.end())
			throw ConversionError(ConversionErrorCode::source_event_not_found,
				"selected COMTRADE trigger event is absent");
		return *found;
	}
	return *std::ranges::min_element(metadata.events, {},
		[](const auto &event) { return event.start_sequence; });
}

BigRational trigger_time(const WaveformMetadata &metadata,
	const EventDescriptor &event)
{
	if ((event.flags & event_trigger_valid) == 0u)
		throw ConversionError(ConversionErrorCode::source_not_ready,
			"COMTRADE trigger event has no exact trigger sequence",
			{"event.trigger_sequence"});
	for (const auto &segment : metadata.timebase_segments) {
		const auto end = cpp_int(segment.first_sequence) + segment.source_frame_count;
		if (event.trigger_sequence >= segment.first_sequence &&
		    cpp_int(event.trigger_sequence) < end)
			return utc_for_sequence(segment, event.trigger_sequence);
	}
	if ((event.flags & event_utc_valid) != 0u &&
	    event.trigger_utc_nanoseconds != 0)
		return {cpp_int(event.trigger_utc_nanoseconds), cpp_int(1)};
	throw ConversionError(ConversionErrorCode::source_not_ready,
		"COMTRADE trigger sequence is outside the selected timebase",
		{"event.trigger_utc_nanoseconds"});
}

std::string offset_text(std::int32_t offset_seconds)
{
	const auto absolute = std::llabs(static_cast<long long>(offset_seconds));
	std::ostringstream output;
	output << (offset_seconds < 0 ? '-' : '+') << absolute / 3600 << 'h'
	       << std::setw(2) << std::setfill('0') << (absolute % 3600) / 60;
	return output.str();
}

std::string time_quality_name(TimeQuality quality)
{
	switch (quality) {
	case TimeQuality::locked: return "A";
	case TimeQuality::holdover: return "B";
	case TimeQuality::unlocked: return "F";
	case TimeQuality::unknown: return "F";
	}
	return "F";
}

void append_u16(std::vector<std::byte> &output, std::uint16_t value)
{
	output.push_back(static_cast<std::byte>(value & 0xffu));
	output.push_back(static_cast<std::byte>((value >> 8u) & 0xffu));
}

void append_u32(std::vector<std::byte> &output, std::uint32_t value)
{
	for (unsigned shift = 0; shift < 32; shift += 8)
		output.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
}

void write_text(OutputSink &sink, std::string_view value)
{
	sink.write(std::as_bytes(std::span(value.data(), value.size())));
}

std::uint64_t event_last_sequence(const EventDescriptor &event)
{
	if ((event.flags & event_end_valid) != 0u)
		return event.end_sequence;
	if ((event.flags & event_current_valid) != 0u)
		return event.current_sequence;
	return event.start_sequence;
}

std::uint64_t event_active_last_sequence(const EventDescriptor &event,
	std::uint64_t selection_last_sequence)
{
	return (event.flags & event_end_valid) != 0u
		? event.end_sequence : selection_last_sequence;
}

std::string hex(std::span<const std::byte> bytes)
{
	std::ostringstream output;
	output << std::hex << std::setfill('0');
	for (const auto byte : bytes)
		output << std::setw(2)
		       << static_cast<unsigned>(std::to_integer<std::uint8_t>(byte));
	return output.str();
}

} // namespace

ConversionSummary ComtradeConverter::convert(const WaveformSource &source,
	OutputSink &sink, const ConversionOptions &options, std::stop_token stop_token,
	ProgressCallback progress) const
{
	if (options.format != ExportFormat::comtrade)
		throw ConversionError(ConversionErrorCode::invalid_options,
			"COMTRADE converter received a different output format");
	validate_conversion_source(source, options);
	check_cancelled(stop_token);
	const auto &metadata = source.metadata();
	std::vector<std::string> missing;
	for (std::size_t index = 0; index < metadata.channels.size(); ++index) {
		const auto &channel = metadata.channels[index];
		if ((channel.flags & channel_transform_valid) == 0u)
			missing.push_back("channel[" + std::to_string(index) + "].affine_transform");
		if ((channel.flags & channel_ratio_valid) == 0u)
			missing.push_back("channel[" + std::to_string(index) + "].primary_secondary_ratio");
		if (channel.gain.denominator == 0 || channel.offset.denominator == 0 ||
		    channel.primary_secondary_ratio.denominator == 0)
			missing.push_back("channel[" + std::to_string(index) + "].rational_denominator");
	}
	for (std::size_t index = 0; index < metadata.timebase_segments.size(); ++index)
		if ((metadata.timebase_segments[index].flags & time_utc_offset_known) == 0u)
			missing.push_back("timebase[" + std::to_string(index) + "].utc_context");
	if (!missing.empty())
		throw ConversionError(ConversionErrorCode::source_not_ready,
			"MNCWF source is not ready for COMTRADE conversion", std::move(missing));
	if (std::ranges::any_of(metadata.timebase_segments,
		[expected = metadata.timebase_segments.front().utc_offset_seconds]
		(const auto &segment) {
			return segment.utc_offset_seconds != expected;
		}))
		throw ConversionError(
			ConversionErrorCode::source_discontinuity_unsupported,
			"COMTRADE interval crosses a captured UTC-offset discontinuity");

	const auto start = utc_for_frame(metadata, 0);
	const auto &trigger = trigger_event(metadata, options);
	const auto trigger_utc = trigger_time(metadata, trigger);
	const auto maximum_relative = subtract(
		utc_for_frame(metadata, source.frame_count() - 1u), start);
	if (maximum_relative.numerator < 0)
		throw ConversionError(
			ConversionErrorCode::source_discontinuity_unsupported,
			"selected waveform UTC timebase moves backwards");
	for (std::size_t index = 1; index != metadata.timebase_segments.size(); ++index) {
		const auto frame = metadata.timebase_segments[index].first_frame;
		const auto boundary_delta = subtract(utc_for_frame(metadata, frame),
			utc_for_frame(metadata, frame - 1u));
		if (boundary_delta.numerator < 0)
			throw ConversionError(
				ConversionErrorCode::source_discontinuity_unsupported,
				"selected waveform UTC timebase moves backwards at a segment boundary");
	}

	/* COMTRADE timestamps are UINT32 * TIMEMULT microseconds. Start at a
	 * nanosecond tick and grow by powers of ten until the complete interval
	 * fits. Half-even rounding bounds error to one half selected tick. */
	std::uint64_t tick_nanoseconds = 1;
	std::uint64_t maximum_tick = 0;
	for (;;) {
		maximum_tick = round_half_even({maximum_relative.numerator,
			maximum_relative.denominator * tick_nanoseconds});
		if (maximum_tick <= std::numeric_limits<std::uint32_t>::max())
			break;
		if (tick_nanoseconds > 1'000'000'000'000ull)
			throw ConversionError(ConversionErrorCode::timestamp_out_of_range,
				"COMTRADE interval cannot fit the BINARY32 timestamp field");
		tick_nanoseconds *= 10u;
	}
	const auto timemult = decimal(tick_nanoseconds, std::uint64_t{1000});

	const auto digital_words = (metadata.events.size() + 15u) / 16u;
	const auto record_bytes = 8ull + 4ull * metadata.channels.size() +
		2ull * digital_words;
	if (record_bytes > std::numeric_limits<std::uint64_t>::max() /
	    source.frame_count())
		throw ConversionError(ConversionErrorCode::output_too_large,
			"COMTRADE DAT size overflows");
	const auto dat_bytes = record_bytes * source.frame_count();

	std::ostringstream cfg;
	cfg << clean(metadata.capture.station_name) << ','
	    << clean(metadata.capture.device_serial.empty()
		? metadata.capture.device_model : metadata.capture.device_serial)
	    << ",2013\r\n";
	cfg << metadata.channels.size() + metadata.events.size() << ','
	    << metadata.channels.size() << "A," << metadata.events.size()
	    << "D\r\n";
	for (std::size_t index = 0; index < metadata.channels.size(); ++index) {
		const auto &channel = metadata.channels[index];
		cfg << index + 1u << ',' << clean(channel.name) << ','
		    << phase_name(channel.phase) << ','
		    << clean(metadata.capture.circuit_name) << ','
		    << unit_name(channel) << ','
		    << decimal(channel.gain.numerator, channel.gain.denominator) << ','
		    << decimal(channel.offset.numerator, channel.offset.denominator)
		    << ",0," << channel.clipping_low << ',' << channel.clipping_high
		    << ',' << channel.primary_secondary_ratio.numerator << ','
		    << channel.primary_secondary_ratio.denominator << ",P\r\n";
	}
	for (std::size_t index = 0; index < metadata.events.size(); ++index) {
		const auto &event = metadata.events[index];
		cfg << index + 1u << ",event-" << uuid_string(event.event_uuid) << ','
		    << ',' << clean(metadata.capture.circuit_name) << ",0\r\n";
	}
	cfg << decimal(metadata.capture.nominal_frequency.numerator,
		metadata.capture.nominal_frequency.denominator) << "\r\n";
	cfg << metadata.timebase_segments.size() << "\r\n";
	for (const auto &segment : metadata.timebase_segments)
		cfg << decimal(segment.persisted_rate.numerator,
			segment.persisted_rate.denominator) << ','
		    << segment.first_frame + segment.frame_count << "\r\n";
	cfg << timestamp_text(start) << "\r\n" << timestamp_text(trigger_utc)
	    << "\r\nBINARY32\r\n" << timemult << "\r\n";
	const auto &first_timebase = metadata.timebase_segments.front();
	cfg << "UTC," << offset_text(first_timebase.utc_offset_seconds) << "\r\n"
	    << time_quality_name(first_timebase.time_quality) << ','
	    << ((first_timebase.flags & (time_positive_leap_pending |
		 time_negative_leap_pending)) != 0u ? 1 : 0) << "\r\n";

	std::ostringstream hdr;
	hdr << "MNCWF source capture "
	    << uuid_string(metadata.capture.source_capture_uuid) << "\r\n"
	    << "Selected capture " << uuid_string(metadata.capture.capture_uuid)
	    << "\r\nCOMTRADE timestamp rounding: ties-to-even, maximum absolute error "
	    << tick_nanoseconds << "/2 ns\r\n"
	    << "Clock uncertainty: " << first_timebase.uncertainty_nanoseconds
	    << " ns; time quality " << time_quality_name(first_timebase.time_quality)
	    << "\r\n";
	if (!metadata.capture.comments.empty())
		hdr << clean(metadata.capture.comments, 2048) << "\r\n";
	for (const auto &entry : metadata.lineage)
		hdr << "Lineage relation=" << static_cast<unsigned>(entry.relation)
		    << " capture=" << uuid_string(entry.related_capture_uuid)
		    << " event=" << uuid_string(entry.related_event_uuid)
		    << " sequences=" << entry.first_sequence << '-'
		    << entry.last_sequence << "\r\n";

	std::ostringstream inf;
	inf << "profile=IEC 60255-24:2013 CFF/BINARY32\r\n"
	    << "source.capture_uuid=" << uuid_string(metadata.capture.source_capture_uuid)
	    << "\r\nselection.capture_uuid=" << uuid_string(metadata.capture.capture_uuid)
	    << "\r\nsource.device_uuid=" << uuid_string(metadata.capture.device_uuid)
	    << "\r\nselection.scope="
	    << (options.scope == ExportScope::capture ? "capture" : "event")
	    << "\r\ntrigger.event_uuid=" << uuid_string(trigger.event_uuid) << "\r\n"
	    << "configuration.sha256=" << hex(metadata.capture.configuration_sha256)
	    << "\r\nsensor_profile.sha256="
	    << hex(metadata.capture.sensor_profile_sha256) << "\r\n"
	    << "configuration.id=" << clean(metadata.capture.configuration_id)
	    << "\r\nsensor_profile.id=" << clean(metadata.capture.sensor_profile_id)
	    << "\r\nfirmware.version=" << clean(metadata.capture.firmware_version)
	    << "\r\nsoftware.build_id=" << clean(metadata.capture.software_build_id)
	    << "\r\ncalibration.id=" << clean(metadata.capture.calibration_id)
	    << "\r\ncalibration.status="
	    << static_cast<unsigned>(metadata.capture.calibration_status) << "\r\n"
	    << "topology=" << static_cast<unsigned>(metadata.capture.topology)
	    << "\r\ntimestamp.tick_nanoseconds=" << tick_nanoseconds << "\r\n"
	    << "timestamp.rounding=half_even\r\n";
	for (std::size_t index = 0; index < metadata.channels.size(); ++index) {
		const auto &channel = metadata.channels[index];
		inf << "channel." << index << ".stable_uuid="
		    << uuid_string(channel.stable_id) << "\r\nchannel." << index
		    << ".source_channel=" << channel.source_channel << "\r\nchannel."
		    << index
		    << ".gain=" << channel.gain.numerator << '/'
		    << channel.gain.denominator << "\r\nchannel." << index
		    << ".offset=" << channel.offset.numerator << '/'
		    << channel.offset.denominator << "\r\nchannel." << index
		    << ".ratio=" << channel.primary_secondary_ratio.numerator << '/'
		    << channel.primary_secondary_ratio.denominator << "\r\nchannel."
		    << index << ".flags=" << channel.flags << "\r\nchannel." << index
		    << ".bits=" << channel.valid_bits << '/' << channel.storage_bits
		    << "\r\nchannel." << index << ".range="
		    << channel.range_minimum.numerator << '/'
		    << channel.range_minimum.denominator << ':'
		    << channel.range_maximum.numerator << '/'
		    << channel.range_maximum.denominator << "\r\nchannel." << index
		    << ".clipping=" << channel.clipping_low << ':'
		    << channel.clipping_high << "\r\n";
	}
	for (std::size_t index = 0; index < metadata.timebase_segments.size(); ++index) {
		const auto &segment = metadata.timebase_segments[index];
		inf << "timebase." << index << ".frames=" << segment.first_frame << '+'
		    << segment.frame_count << "\r\ntimebase." << index
		    << ".sequences=" << segment.first_sequence << '+'
		    << segment.source_frame_count << "\r\ntimebase." << index
		    << ".sequence_step=" << segment.sequence_step << "\r\ntimebase."
		    << index << ".acquisition_rate="
		    << segment.acquisition_rate.numerator << '/'
		    << segment.acquisition_rate.denominator << "\r\ntimebase." << index
		    << ".persisted_rate=" << segment.persisted_rate.numerator << '/'
		    << segment.persisted_rate.denominator << "\r\ntimebase." << index
		    << ".decimation=" << segment.decimation_divisor << ':'
		    << static_cast<unsigned>(segment.decimation_method)
		    << "\r\ntimebase." << index << ".correlation="
		    << segment.correlation_sequence << ':'
		    << segment.correlation_utc_nanoseconds << "\r\ntimebase." << index
		    << ".clock=" << static_cast<unsigned>(segment.clock_source) << ':'
		    << static_cast<unsigned>(segment.time_quality) << ':'
		    << segment.uncertainty_nanoseconds << "\r\ntimebase." << index
		    << ".utc_offset_seconds=" << segment.utc_offset_seconds
		    << "\r\ntimebase." << index << ".flags=" << segment.flags << "\r\n";
	}
	for (const auto &event : metadata.events)
		inf << "event=" << uuid_string(event.event_uuid)
		    << ",start=" << event.start_sequence
		    << ",end=" << event_last_sequence(event)
		    << ",flags=" << event.flags << ",status=" << event.status
		    << ",lifecycle=" << static_cast<unsigned>(event.lifecycle)
		    << ",phase_mask=" << event.phase_mask
		    << ",trigger_source=" << event.trigger_source
		    << ",severity=" << event.severity
		    << ",uncertainty_ns=" << event.uncertainty_nanoseconds
		    << ",reference_u=" << event.reference_micro_units
		    << ",threshold_u=" << event.threshold_micro_units
		    << ",hysteresis_u=" << event.hysteresis_micro_units
		    << ",extrema_u=" << event.extrema_micro_units[0] << ':'
		    << event.extrema_micro_units[1] << ':'
		    << event.extrema_micro_units[2]
		    << ",duration_samples=" << event.duration_samples
		    << ",updates=" << event.update_count
		    << ",configuration_generation=" << event.configuration_generation
		    << ",taxonomy=" << clean(event.taxonomy_name)
		    << ",label=" << clean(event.label) << "\r\n"
		    << "event.settings." << uuid_string(event.event_uuid) << '='
		    << clean_line(event.settings_snapshot_json) << "\r\n";
	for (const auto &quality : metadata.quality_intervals)
		inf << "quality=frames:" << quality.first_frame << '+'
		    << quality.frame_count << ",sequences:" << quality.first_sequence
		    << '-' << quality.last_sequence << ",flags:" << quality.flags
		    << ",channels:" << quality.channel_mask << ",severity:"
		    << quality.severity << ",source:" << quality.source
		    << ",detail:" << quality.detail_code << "\r\n";
	for (const auto &entry : metadata.lineage)
		inf << "lineage=relation:" << static_cast<unsigned>(entry.relation)
		    << ",capture:" << uuid_string(entry.related_capture_uuid)
		    << ",event:" << uuid_string(entry.related_event_uuid)
		    << ",sequences:" << entry.first_sequence << '-'
		    << entry.last_sequence << ",part:" << entry.part_index << '/'
		    << entry.part_count << "\r\n";

	const auto cfg_text = cfg.str();
	const auto hdr_text = hdr.str();
	const auto inf_text = inf.str();
	const std::string prefix = "--- file type: CFG ---\r\n" + cfg_text +
		"--- file type: INF ---\r\n" + inf_text +
		"--- file type: HDR ---\r\n" + hdr_text +
		"--- file type: DAT BINARY32: " + std::to_string(dat_bytes) +
		" ---\r\n";
	if (dat_bytes > options.maximum_output_bytes || prefix.size() >
	    options.maximum_output_bytes - dat_bytes ||
	    dat_bytes > sink.byte_limit() || prefix.size() > sink.byte_limit() - dat_bytes)
		throw ConversionError(ConversionErrorCode::output_too_large,
			"COMTRADE export exceeds its configured output limit");
	write_text(sink, prefix);

	if (progress)
		progress({0, source.frame_count()});
	const auto batch_frames = std::min<std::uint64_t>(options.frame_batch_size,
		source.frame_count());
	std::vector<std::int64_t> samples(static_cast<std::size_t>(batch_frames) *
		source.channel_count());
	std::vector<std::byte> records;
	records.reserve(static_cast<std::size_t>(batch_frames * record_bytes));
	std::uint64_t processed = 0;
	while (processed < source.frame_count()) {
		check_cancelled(stop_token);
		const auto requested = static_cast<std::size_t>(std::min<std::uint64_t>(
			batch_frames, source.frame_count() - processed));
		const auto produced = source.read_frames(processed, requested,
			std::span<std::int64_t>{samples}.first(requested * source.channel_count()));
		if (produced == 0 || produced > requested)
			throw ConversionError(ConversionErrorCode::source_invalid,
				"waveform source stopped before the declared frame count");
		records.clear();
		for (std::size_t local = 0; local < produced; ++local) {
			const auto frame = processed + local;
			if (frame + 1u > std::numeric_limits<std::uint32_t>::max())
				throw ConversionError(ConversionErrorCode::sample_out_of_range,
					"COMTRADE sample number exceeds UINT32");
			append_u32(records, static_cast<std::uint32_t>(frame + 1u));
			const auto relative = subtract(utc_for_frame(metadata, frame), start);
			const auto ticks = round_half_even({relative.numerator,
				relative.denominator * tick_nanoseconds});
			if (ticks > std::numeric_limits<std::uint32_t>::max())
				throw ConversionError(ConversionErrorCode::timestamp_out_of_range,
					"COMTRADE frame timestamp exceeds UINT32");
			append_u32(records, static_cast<std::uint32_t>(ticks));
			for (std::size_t channel = 0; channel < source.channel_count(); ++channel) {
				const auto value = samples[local * source.channel_count() + channel];
				if (value < std::numeric_limits<std::int32_t>::min() ||
				    value > std::numeric_limits<std::int32_t>::max())
					throw ConversionError(ConversionErrorCode::sample_out_of_range,
						"source sample cannot be represented by COMTRADE BINARY32");
				append_u32(records, std::bit_cast<std::uint32_t>(
					static_cast<std::int32_t>(value)));
			}
			const auto [first_sequence, last_sequence] =
				sequence_range_for_frame(metadata, frame);
			for (std::size_t word = 0; word < digital_words; ++word) {
				std::uint16_t bits = 0;
				for (std::size_t bit = 0; bit < 16; ++bit) {
					const auto event_index = word * 16u + bit;
					if (event_index >= metadata.events.size())
						break;
					const auto &event = metadata.events[event_index];
					if (first_sequence <= event_active_last_sequence(
						    event, metadata.last_sequence) &&
					    last_sequence >= event.start_sequence)
						bits |= static_cast<std::uint16_t>(1u << bit);
				}
				append_u16(records, bits);
			}
		}
		sink.write(records);
		processed += produced;
		if (progress)
			progress({processed, source.frame_count()});
	}
	return {ExportFormat::comtrade, "IEC 60255-24:2013 CFF/BINARY32",
		source.frame_count(), sink.bytes_written(), ".cff",
		"application/vnd.iec.comtrade"};
}

} // namespace mnc::waveform
