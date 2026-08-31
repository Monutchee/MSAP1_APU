#include "msap1/datalogger/msap1_datalogger.hpp"

#include <algorithm>
#include <array>

namespace msap1::datalogger {
namespace {

mnc::meter::ReadingQuality quality(msap1::MeasurementQuality source)
{
	switch (source) {
	case msap1::MeasurementQuality::unavailable:
		return mnc::meter::ReadingQuality::Unavailable;
	case msap1::MeasurementQuality::valid:
		return mnc::meter::ReadingQuality::Valid;
	case msap1::MeasurementQuality::invalid:
		return mnc::meter::ReadingQuality::Invalid;
	case msap1::MeasurementQuality::out_of_range:
		return mnc::meter::ReadingQuality::OutOfRange;
	case msap1::MeasurementQuality::timed_out:
		return mnc::meter::ReadingQuality::TimedOut;
	case msap1::MeasurementQuality::arithmetic_error:
		return mnc::meter::ReadingQuality::ArithmeticError;
	}
	return mnc::meter::ReadingQuality::Unavailable;
}

std::int64_t nominal_interval(mnc::meter::MeasurementPeriod period)
{
	using Period = mnc::meter::MeasurementPeriod;
	switch (period) {
	case Period::Basic: return 200'000'000ll;
	case Period::Cycles150_180: return 3'000'000'000ll;
	case Period::Min10: return 600'000'000'000ll;
	case Period::Hour2: return 7'200'000'000'000ll;
	case Period::Demand: return 600'000'000'000ll;
	case Period::Min10Live:
	case Period::Hour2Live:
		break;
	}
	throw mnc::datalogger::DatalogError(
		mnc::datalogger::DatalogErrorCode::UnsupportedCapability,
		"open interval previews cannot generate datalogs");
}

std::int64_t query_chunk(mnc::meter::MeasurementPeriod period)
{
	using Period = mnc::meter::MeasurementPeriod;
	switch (period) {
	case Period::Basic: return 30'000'000'000ll;
	case Period::Cycles150_180: return 300'000'000'000ll;
	case Period::Min10:
	case Period::Demand: return 3'600'000'000'000ll;
	case Period::Hour2: return 28'800'000'000'000ll;
	case Period::Min10Live:
	case Period::Hour2Live:
		break;
	}
	return 30'000'000'000ll;
}

mnc::meter_stream::DatabaseDataset dataset(
	mnc::meter::MeasurementPeriod period)
{
	using Dataset = mnc::meter_stream::DatabaseDataset;
	using Period = mnc::meter::MeasurementPeriod;
	switch (period) {
	case Period::Basic: return Dataset::basic;
	case Period::Cycles150_180: return Dataset::cycles_150_180;
	case Period::Min10: return Dataset::minutes_10;
	case Period::Hour2: return Dataset::hours_2;
	case Period::Demand: return Dataset::demand;
	case Period::Min10Live:
	case Period::Hour2Live: break;
	}
	throw mnc::datalogger::DatalogError(
		mnc::datalogger::DatalogErrorCode::UnsupportedCapability,
		"open interval previews cannot generate datalogs");
}

} // namespace

std::vector<mnc::datalogger::HistoricalSample>
Msap1HistorianDataSource::query(mnc::meter::MeasurementPeriod period,
	std::span<const mnc::meter::MeterAttributeKey> attributes,
	mnc::datalogger::UtcWindow window) const
{
	if (!window.valid())
		throw std::invalid_argument("historian query window is invalid");
	try {
		const auto status = historian_.status();
		if (!status.healthy || status.migration_in_progress ||
		    status.backfill_incomplete)
			throw mnc::datalogger::DatalogError(
				mnc::datalogger::DatalogErrorCode::SourceUnavailable,
				"historian is not ready for Data Sender generation");
		const auto wanted = dataset(period);
		const auto available = std::ranges::find(status.datasets, wanted,
			&msap1::history::HistorianStatus::DatasetStatus::dataset);
		if (available == status.datasets.end())
			throw mnc::datalogger::DatalogError(
				mnc::datalogger::DatalogErrorCode::SourceUnavailable,
				"historian does not report the selected dataset");
		if (available->oldest_nanoseconds &&
		    window.start < *available->oldest_nanoseconds)
			throw mnc::datalogger::DatalogError(
				mnc::datalogger::DatalogErrorCode::SourceRetentionGap,
				"source window predates retained historian data");
	} catch (const mnc::datalogger::DatalogError &) {
		throw;
	} catch (const std::exception &error) {
		throw mnc::datalogger::DatalogError(
			mnc::datalogger::DatalogErrorCode::SourceUnavailable,
			std::string("historian status failed: ") + error.what());
	}
	msap1::history::HistoryQuery request;
	request.period = period;
	request.limit = 50000;
	for (const auto attribute : attributes) {
		if (attribute.index)
			throw std::invalid_argument(
				"indexed historian attribute is unsupported");
		request.attributes.push_back(attribute.id);
	}

	std::vector<mnc::datalogger::HistoricalSample> result;
	const auto chunk_size = query_chunk(period);
	for (auto start = window.start; start < window.end; start += chunk_size) {
		request.start_nanoseconds = start;
		request.end_nanoseconds = std::min(window.end, start + chunk_size);
		request.after.reset();
		for (;;) {
			const auto page = historian_.query(request);
			for (const auto &point : page) {
				result.push_back({
					.measured_at = point.measured_at_nanoseconds,
					.source_sequence = point.source_sequence,
					.attribute = {point.attribute, std::nullopt},
					.value = point.value,
					.quality = quality(point.quality),
					.reset_epoch = point.reset_epoch,
				});
			}
			if (page.size() < request.limit)
				break;
			if (page.empty())
				throw mnc::datalogger::DatalogError(
					mnc::datalogger::DatalogErrorCode::SourceUnavailable,
					"historian continuation made no progress");
			request.after = page.back().cursor;
		}
	}
	return result;
}

std::uint64_t Msap1HistorianDataSource::expected_sample_count(
	mnc::meter::MeasurementPeriod period,
	mnc::meter::MeterAttributeKey,
	mnc::datalogger::UtcWindow window) const
{
	const auto interval = nominal_interval(period);
	return window.duration_nanoseconds() <= 0
		? 0u
		: static_cast<std::uint64_t>(
			window.duration_nanoseconds() / interval);
}

mnc::datalogger::GeneratedDataset Msap1Datalogger::generate(
	const mnc::datalogger::DatalogJobSnapshot &job,
	mnc::datalogger::UtcWindow completed_window,
	mnc::datalogger::UtcNanoseconds generated_at) const
{
	const auto minimum = nominal_interval(job.source_period);
	if (job.row_interval_nanoseconds < minimum ||
	    job.row_interval_nanoseconds % minimum != 0)
		throw mnc::datalogger::DatalogError(
			mnc::datalogger::DatalogErrorCode::InvalidConfiguration,
			"row interval is incompatible with the source period");
	return generator_.generate(job, completed_window, generated_at);
}

} // namespace msap1::datalogger
