#include "mnc/datalogger/datalogger.hpp"

#include <boost/multiprecision/cpp_int.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <ranges>
#include <set>
#include <sstream>
#include <tuple>

namespace mnc::datalogger {
namespace {

using boost::multiprecision::cpp_int;
using Calculation = mnc::meter::MeterAttributeCalculation;
using Quality = mnc::meter::ReadingQuality;

bool safe_id(std::string_view value)
{
	if (value.empty() || value.size() > 128)
		return false;
	return std::ranges::all_of(value, [](unsigned char character) {
		return std::isalnum(character) != 0 || character == '-' ||
			character == '_';
	});
}

bool supports_calculation(const mnc::meter::MeterAttributeDescriptor &descriptor,
	Calculation calculation)
{
	return std::ranges::find(descriptor.calculations, calculation) !=
		descriptor.calculations.end();
}

std::string decimal_ratio(cpp_int numerator, std::uint64_t denominator)
{
	if (denominator == 0)
		throw DatalogError(DatalogErrorCode::InvalidConfiguration,
			"average denominator is zero");
	const bool negative = numerator < 0;
	if (negative)
		numerator = -numerator;
	const cpp_int divisor = denominator;
	const cpp_int integer = numerator / divisor;
	cpp_int remainder = numerator % divisor;
	std::string result = integer.convert_to<std::string>();
	std::string fraction;
	for (int digit = 0; digit < 6 && remainder != 0; ++digit) {
		remainder *= 10;
		const cpp_int quotient = remainder / divisor;
		const auto next = quotient.convert_to<unsigned>();
		fraction.push_back(static_cast<char>('0' + next));
		remainder %= divisor;
	}
	while (!fraction.empty() && fraction.back() == '0')
		fraction.pop_back();
	if (!fraction.empty())
		result += "." + fraction;
	if (negative && result != "0")
		result.insert(result.begin(), '-');
	return result;
}

std::string decimal_angle(long double value)
{
	std::ostringstream output;
	output.imbue(std::locale::classic());
	output << std::fixed << std::setprecision(6) << value;
	auto result = output.str();
	while (result.contains('.') && result.back() == '0')
		result.pop_back();
	if (!result.empty() && result.back() == '.')
		result.pop_back();
	return result;
}

Quality empty_quality(std::span<const HistoricalSample *const> samples)
{
	for (const auto *sample : samples)
		if (sample->quality != Quality::Unavailable)
			return sample->quality;
	return Quality::Unavailable;
}

DatasetCell aggregate(std::span<const HistoricalSample *const> samples,
	Calculation calculation, std::uint64_t expected)
{
	DatasetCell result;
	result.expected_samples = expected;
	std::vector<const HistoricalSample *> valid;
	valid.reserve(samples.size());
	for (const auto *sample : samples)
		if (sample->quality == Quality::Valid)
			valid.push_back(sample);
	result.contributing_samples = valid.size();
	result.complete = expected != 0 && valid.size() >= expected;
	if (valid.empty()) {
		result.quality = empty_quality(samples);
		return result;
	}
	std::ranges::sort(valid, [](const auto *left, const auto *right) {
		return std::tie(left->measured_at, left->source_sequence) <
			std::tie(right->measured_at, right->source_sequence);
	});
	result.quality = Quality::Valid;

	switch (calculation) {
	case Calculation::Minimum: {
		const auto point = *std::ranges::min_element(valid, {},
			&HistoricalSample::value);
		result.value = std::to_string(point->value);
		break;
	}
	case Calculation::Maximum: {
		const auto point = *std::ranges::max_element(valid, {},
			&HistoricalSample::value);
		result.value = std::to_string(point->value);
		break;
	}
	case Calculation::Average: {
		cpp_int sum = 0;
		for (const auto *point : valid)
			sum += point->value;
		result.value = decimal_ratio(sum, valid.size());
		break;
	}
	case Calculation::CircularAverage: {
		constexpr long double pi = 3.141592653589793238462643383279502884L;
		long double sine = 0;
		long double cosine = 0;
		for (const auto *point : valid) {
			const auto radians =
				(static_cast<long double>(point->value) / 1000.0L) *
				pi / 180.0L;
			sine += std::sin(radians);
			cosine += std::cos(radians);
		}
		if (std::hypot(sine, cosine) <
		    std::numeric_limits<long double>::epsilon() * valid.size()) {
			result.quality = Quality::ArithmeticError;
			result.value.reset();
			break;
		}
		auto degrees = std::atan2(sine, cosine) * 180.0L / pi;
		if (degrees < 0)
			degrees += 360.0L;
		result.value = decimal_angle(degrees * 1000.0L);
		break;
	}
	case Calculation::First:
		result.value = std::to_string(valid.front()->value);
		result.reset_epoch = valid.front()->reset_epoch;
		break;
	case Calculation::Last:
		result.value = std::to_string(valid.back()->value);
		result.reset_epoch = valid.back()->reset_epoch;
		break;
	case Calculation::Delta:
		if (!valid.front()->reset_epoch || !valid.back()->reset_epoch ||
		    valid.front()->reset_epoch != valid.back()->reset_epoch) {
			result.value.reset();
			result.quality = Quality::Unavailable;
			result.continuity = false;
			break;
		}
		result.value = (cpp_int(valid.back()->value) -
			cpp_int(valid.front()->value)).convert_to<std::string>();
		result.reset_epoch = valid.back()->reset_epoch;
		break;
	}
	return result;
}

} // namespace

std::string_view content_format_name(ContentFormat format) noexcept
{
	switch (format) {
	case ContentFormat::Json: return "json";
	case ContentFormat::Csv: return "csv";
	}
	return "unknown";
}

std::string_view calculation_name(Calculation calculation) noexcept
{
	switch (calculation) {
	case Calculation::Minimum: return "minimum";
	case Calculation::Maximum: return "maximum";
	case Calculation::Average: return "average";
	case Calculation::Last: return "last";
	case Calculation::CircularAverage: return "circular_average";
	case Calculation::First: return "first";
	case Calculation::Delta: return "delta";
	}
	return "unknown";
}

std::string_view quality_name(Quality quality) noexcept
{
	switch (quality) {
	case Quality::Unavailable: return "unavailable";
	case Quality::Valid: return "valid";
	case Quality::Invalid: return "invalid";
	case Quality::OutOfRange: return "out_of_range";
	case Quality::TimedOut: return "timed_out";
	case Quality::ArithmeticError: return "arithmetic_error";
	}
	return "unavailable";
}

std::string_view period_name(mnc::meter::MeasurementPeriod period) noexcept
{
	for (const auto &descriptor : mnc::meter::defined_measurement_periods())
		if (descriptor.period == period)
			return descriptor.key;
	return "unknown";
}

GeneratedDataset HistoricalDatasetGenerator::generate(
	const DatalogJobSnapshot &job, UtcWindow completed_window,
	UtcNanoseconds generated_at) const
{
	if (!safe_id(job.job_id) || job.revision == 0 || job.product_id.empty() ||
	    job.device_id.empty())
		throw DatalogError(DatalogErrorCode::InvalidConfiguration,
			"datalog job identity is invalid");
	if (!completed_window.valid() ||
	    completed_window.duration_nanoseconds() !=
		job.generation_interval_nanoseconds ||
	    job.row_interval_nanoseconds <= 0 ||
	    job.generation_interval_nanoseconds % job.row_interval_nanoseconds != 0)
		throw DatalogError(DatalogErrorCode::InvalidConfiguration,
			"generation and row intervals are incompatible");
	if (job.selections.empty())
		throw DatalogError(DatalogErrorCode::InvalidConfiguration,
			"datalog job has no selected attributes");

	GeneratedDataset result;
	result.job_id = job.job_id;
	result.job_revision = job.revision;
	result.product_id = job.product_id;
	result.device_id = job.device_id;
	result.source_period = job.source_period;
	result.format = job.format;
	result.generated_at = generated_at;
	result.artifact_window = completed_window;
	result.artifact_id = job.job_id + "-r" + std::to_string(job.revision) +
		"-" + std::to_string(completed_window.start) + "-" +
		std::to_string(completed_window.end) + "-" +
		std::string(content_format_name(job.format));

	std::vector<mnc::meter::MeterAttributeKey> attributes;
	std::set<std::pair<std::uint16_t, Calculation>> selected;
	for (const auto &selection : job.selections) {
		const auto descriptor = mnc::meter::describe(selection.attribute);
		if (!mnc::meter::supports_attribute(selection.attribute,
				job.source_period,
				mnc::meter::MeterAttributeUsage::Historian) ||
		    !supports_calculation(descriptor, selection.calculation))
			throw DatalogError(DatalogErrorCode::UnsupportedCapability,
				"selected meter attribute/calculation is unsupported");
		const auto identity = std::pair{
			static_cast<std::uint16_t>(selection.attribute.id),
			selection.calculation};
		if (!selected.insert(identity).second)
			throw DatalogError(DatalogErrorCode::InvalidConfiguration,
				"duplicate datalog column selection");
		if (std::ranges::find(attributes, selection.attribute) == attributes.end())
			attributes.push_back(selection.attribute);
		result.columns.push_back({
			.id = std::string(descriptor.key) + "." +
				std::string(calculation_name(selection.calculation)),
			.attribute_key = std::string(descriptor.key),
			.label = std::string(descriptor.label),
			.unit = std::string(mnc::meter::unit_name(descriptor.unit)),
			.calculation = selection.calculation,
			.value_kind = descriptor.value_kind,
		});
	}

	std::vector<HistoricalSample> samples;
	try {
		samples = source_.query(job.source_period, attributes, completed_window);
	} catch (const DatalogError &) {
		throw;
	} catch (const std::exception &error) {
		throw DatalogError(DatalogErrorCode::SourceUnavailable,
			std::string("historian query failed: ") + error.what());
	}
	std::ranges::sort(samples, [](const auto &left, const auto &right) {
		return std::tie(left.measured_at, left.source_sequence,
			left.attribute.id, left.attribute.index) <
			std::tie(right.measured_at, right.source_sequence,
				right.attribute.id, right.attribute.index);
	});

	for (auto start = completed_window.start; start < completed_window.end;
	     start += job.row_interval_nanoseconds) {
		DatasetRow row{.window = {start,
			start + job.row_interval_nanoseconds}, .cells = {}};
		row.cells.reserve(job.selections.size());
		for (const auto &selection : job.selections) {
			std::vector<const HistoricalSample *> bucket;
			for (const auto &sample : samples)
				if (sample.attribute == selection.attribute &&
				    sample.measured_at >= row.window.start &&
				    sample.measured_at < row.window.end)
					bucket.push_back(&sample);
			const auto expected = source_.expected_sample_count(
				job.source_period, selection.attribute, row.window);
			row.cells.push_back(aggregate(bucket,
				selection.calculation, expected));
		}
		result.rows.push_back(std::move(row));
	}
	return result;
}

} // namespace mnc::datalogger
