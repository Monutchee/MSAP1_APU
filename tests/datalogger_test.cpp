#include "mnc/datalogger/datalogger.hpp"
#include "mnc/datalogger/meter_data_content_writer.hpp"

#include <climits>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {

using namespace mnc::datalogger;
using mnc::meter::MeterAttributeCalculation;
using mnc::meter::MeterAttributeId;
using mnc::meter::MeterAttributeKey;
using mnc::meter::MeasurementPeriod;
using mnc::meter::ReadingQuality;

void require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

class FakeHistory final : public HistoricalMeterDataSource {
public:
	std::vector<HistoricalSample> samples;
	bool retention_gap = false;

	std::vector<HistoricalSample> query(MeasurementPeriod,
		std::span<const MeterAttributeKey> attributes,
		UtcWindow window) const override
	{
		if (retention_gap)
			throw DatalogError(DatalogErrorCode::SourceRetentionGap,
				"source window was pruned");
		std::vector<HistoricalSample> result;
		for (const auto &sample : samples) {
			if (sample.measured_at < window.start ||
			    sample.measured_at >= window.end)
				continue;
			for (const auto attribute : attributes)
				if (attribute == sample.attribute) {
					result.push_back(sample);
					break;
				}
		}
		return result;
	}

	std::uint64_t expected_sample_count(MeasurementPeriod period,
		MeterAttributeKey, UtcWindow window) const override
	{
		const auto interval = period == MeasurementPeriod::Basic
			? 200'000'000ll : 600'000'000'000ll;
		return static_cast<std::uint64_t>(
			window.duration_nanoseconds() / interval);
	}
};

DatalogJobSnapshot basic_job()
{
	DatalogJobSnapshot job;
	job.job_id = "canonical-five-minute";
	job.revision = 3;
	job.product_id = "msap1";
	job.device_id = "meter-01";
	job.source_period = MeasurementPeriod::Basic;
	job.generation_interval_nanoseconds = 300'000'000'000ll;
	job.row_interval_nanoseconds = 60'000'000'000ll;
	job.format = ContentFormat::Json;
	job.selections = {
		{{MeterAttributeId::VanRms, std::nullopt},
		 MeterAttributeCalculation::Minimum},
		{{MeterAttributeId::VanRms, std::nullopt},
		 MeterAttributeCalculation::Maximum},
		{{MeterAttributeId::VanRms, std::nullopt},
		 MeterAttributeCalculation::Average},
	};
	return job;
}

void canonical_five_row_example()
{
	FakeHistory source;
	for (std::int64_t sample = 0; sample < 1500; ++sample) {
		source.samples.push_back({
			.measured_at = sample * 200'000'000ll,
			.source_sequence = static_cast<std::uint64_t>(sample + 1),
			.attribute = {MeterAttributeId::VanRms, std::nullopt},
			.value = sample % 300,
			.quality = ReadingQuality::Valid,
			.reset_epoch = std::nullopt,
		});
	}
	HistoricalDatasetGenerator generator(source);
	const auto dataset = generator.generate(basic_job(),
		{0, 300'000'000'000ll}, 301'000'000'000ll);
	require(dataset.rows.size() == 5 && dataset.columns.size() == 3,
		"canonical job did not produce five wide rows");
	for (const auto &row : dataset.rows) {
		require(row.cells[0].value == "0" &&
			row.cells[1].value == "299" &&
			row.cells[2].value == "149.5",
			"linear aggregation returned the wrong exact value");
		for (const auto &cell : row.cells)
			require(cell.quality == ReadingQuality::Valid && cell.complete &&
				cell.contributing_samples == 300 &&
				cell.expected_samples == 300,
				"canonical row coverage is incorrect");
	}
	require(dataset.artifact_id ==
		"canonical-five-minute-r3-0-300000000000-json",
		"artifact identity is not deterministic");
}

void average_is_overflow_safe()
{
	FakeHistory source;
	source.samples = {
		{0, 1, {MeterAttributeId::VanRms, std::nullopt},
		 INT64_MAX, ReadingQuality::Valid, std::nullopt},
		{200'000'000ll, 2, {MeterAttributeId::VanRms, std::nullopt},
		 INT64_MIN, ReadingQuality::Valid, std::nullopt},
	};
	auto job = basic_job();
	job.generation_interval_nanoseconds = 60'000'000'000ll;
	job.selections = {{{MeterAttributeId::VanRms, std::nullopt},
		MeterAttributeCalculation::Average}};
	HistoricalDatasetGenerator generator(source);
	const auto dataset = generator.generate(job, {0, 60'000'000'000ll}, 0);
	require(dataset.rows[0].cells[0].value == "-0.5",
		"overflow-safe signed average changed");
}

void circular_angles_cross_zero_correctly()
{
	FakeHistory source;
	source.samples = {
		{0, 1, {MeterAttributeId::VoltagePhaseAngleA, std::nullopt},
		 359000, ReadingQuality::Valid, std::nullopt},
		{200'000'000ll, 2,
		 {MeterAttributeId::VoltagePhaseAngleA, std::nullopt},
		 1000, ReadingQuality::Valid, std::nullopt},
	};
	auto job = basic_job();
	job.generation_interval_nanoseconds = 60'000'000'000ll;
	job.selections = {{{MeterAttributeId::VoltagePhaseAngleA, std::nullopt},
		MeterAttributeCalculation::CircularAverage}};
	HistoricalDatasetGenerator generator(source);
	const auto dataset = generator.generate(job, {0, 60'000'000'000ll}, 0);
	const auto angle = std::stold(*dataset.rows[0].cells[0].value);
	require(std::fabs(angle) < 0.001L ||
		std::fabs(angle - 360000.0L) < 0.001L,
		"circular mean used a linear angle average");
	require(!dataset.rows[0].cells[0].complete,
		"partial source coverage was not reported");
}

void energy_delta_breaks_at_reset_epoch()
{
	FakeHistory source;
	source.samples = {
		{0, 1, {MeterAttributeId::ActiveImportEnergyTotal, std::nullopt},
		 100, ReadingQuality::Valid, 7},
		{600'000'000'000ll, 2,
		 {MeterAttributeId::ActiveImportEnergyTotal, std::nullopt},
		 10, ReadingQuality::Valid, 8},
	};
	DatalogJobSnapshot job;
	job.job_id = "energy-reset";
	job.revision = 1;
	job.product_id = "msap1";
	job.device_id = "meter-01";
	job.source_period = MeasurementPeriod::Min10;
	job.generation_interval_nanoseconds = 1'200'000'000'000ll;
	job.row_interval_nanoseconds = job.generation_interval_nanoseconds;
	job.selections = {{{MeterAttributeId::ActiveImportEnergyTotal,
		std::nullopt}, MeterAttributeCalculation::Delta}};
	HistoricalDatasetGenerator generator(source);
	const auto dataset = generator.generate(job,
		{0, 1'200'000'000'000ll}, 0);
	const auto &cell = dataset.rows[0].cells[0];
	require(!cell.value && !cell.continuity &&
		cell.quality == ReadingQuality::Unavailable,
		"energy reset was rendered as consumption");
}

void empty_partial_and_exact_boundary_rows_are_explicit()
{
	FakeHistory source;
	auto job = basic_job();
	job.generation_interval_nanoseconds = 120'000'000'000ll;
	job.row_interval_nanoseconds = 60'000'000'000ll;
	job.selections = {{{MeterAttributeId::VanRms, std::nullopt},
		MeterAttributeCalculation::Last}};
	HistoricalDatasetGenerator generator(source);
	auto empty = generator.generate(job, {0, 120'000'000'000ll}, 0);
	require(empty.rows.size() == 2 && !empty.rows[0].cells[0].value &&
		empty.rows[0].cells[0].quality == ReadingQuality::Unavailable &&
		empty.rows[0].cells[0].contributing_samples == 0,
		"no-data bucket did not retain explicit unavailable quality");

	source.samples = {
		{59'999'999'999ll, 1,
			{MeterAttributeId::VanRms, std::nullopt}, 10,
			ReadingQuality::Valid, std::nullopt},
		{60'000'000'000ll, 2,
			{MeterAttributeId::VanRms, std::nullopt}, 20,
			ReadingQuality::Valid, std::nullopt},
		{60'000'000'000ll, 3,
			{MeterAttributeId::VanRms, std::nullopt}, 0,
			ReadingQuality::Valid, std::nullopt},
		{120'000'000'000ll, 4,
			{MeterAttributeId::VanRms, std::nullopt}, 99,
			ReadingQuality::Valid, std::nullopt},
	};
	auto bounded = generator.generate(job, {0, 120'000'000'000ll}, 0);
	require(bounded.rows[0].cells[0].value == "10" &&
		bounded.rows[1].cells[0].value == "0" &&
		bounded.rows[1].cells[0].contributing_samples == 2 &&
		!bounded.rows[1].cells[0].complete,
		"half-open row boundaries or same-time sequence ordering changed");
}

void source_retention_gap_is_not_serialized_as_empty_data()
{
	FakeHistory source;
	source.retention_gap = true;
	HistoricalDatasetGenerator generator(source);
	bool propagated = false;
	try {
		(void)generator.generate(basic_job(),
			{0, 300'000'000'000ll}, 0);
	} catch (const DatalogError &error) {
		propagated = error.code() == DatalogErrorCode::SourceRetentionGap;
	}
	require(propagated,
		"source-retention gap was converted into a false empty artifact");
}

void writers_are_injected_deterministic_and_exact()
{
	FakeHistory source;
	source.samples.push_back({0, 1,
		{MeterAttributeId::VanRms, std::nullopt}, 0,
		ReadingQuality::Valid, std::nullopt});
	auto job = basic_job();
	job.generation_interval_nanoseconds = 60'000'000'000ll;
	job.selections = {{{MeterAttributeId::VanRms, std::nullopt},
		MeterAttributeCalculation::Last}};
	HistoricalDatasetGenerator generator(source);
	DefaultMeterDataContentWriterFactory factory;
	auto json_dataset = generator.generate(job, {0, 60'000'000'000ll}, 0);
	const auto json_writer = factory.create(ContentFormat::Json);
	const auto first = json_writer->write(json_dataset);
	const auto second = json_writer->write(json_dataset);
	require(first.body == second.body && first.sha256 == second.sha256 &&
		first.sha256.size() == 64 && first.extension == "json" &&
		first.mime_type == "application/json" &&
		first.body.find("\"value\": \"0\"") != std::string::npos,
		"JSON writer is not deterministic or lost valid zero");

	job.format = ContentFormat::Csv;
	auto csv_dataset = generator.generate(job, {0, 60'000'000'000ll}, 0);
	const auto csv = factory.create(ContentFormat::Csv)->write(csv_dataset);
	require(csv.extension == "csv" &&
		csv.mime_type == "text/csv; charset=utf-8" &&
		csv.body.find("\r\n") != std::string::npos &&
		csv.body.find("voltage.ln.a.rms.last.value") != std::string::npos,
		"CSV writer contract changed");

	json_dataset.product_id = "product\"name";
	json_dataset.device_id = "meter,one";
	const auto escaped_json = json_writer->write(json_dataset);
	require(escaped_json.body.find("product\\\"name") != std::string::npos,
		"JSON writer did not escape identity text");
	csv_dataset.product_id = "product\"name";
	csv_dataset.device_id = "meter,one";
	const auto escaped_csv = factory.create(ContentFormat::Csv)->write(csv_dataset);
	require(escaped_csv.body.find("\"product\"\"name\"") !=
		std::string::npos &&
		escaped_csv.body.find("\"meter,one\"") != std::string::npos,
		"CSV writer did not apply RFC 4180 escaping");
}

} // namespace

int main()
{
	try {
		canonical_five_row_example();
		average_is_overflow_safe();
		circular_angles_cross_zero_correctly();
		energy_delta_breaks_at_reset_epoch();
		empty_partial_and_exact_boundary_rows_are_explicit();
		source_retention_gap_is_not_serialized_as_empty_data();
		writers_are_injected_deterministic_and_exact();
		std::cout << "datalogger tests passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "datalogger test failed: " << error.what() << '\n';
		return 1;
	}
}
