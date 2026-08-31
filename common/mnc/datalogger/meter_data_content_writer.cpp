#include "mnc/datalogger/meter_data_content_writer.hpp"

#include <openssl/sha.h>

#include <array>
#include <iomanip>
#include <memory>
#include <sstream>

namespace mnc::datalogger {
namespace {

std::string json_string(std::string_view value)
{
	std::string result{"\""};
	for (const unsigned char character : value) {
		switch (character) {
		case '"': result += "\\\""; break;
		case '\\': result += "\\\\"; break;
		case '\b': result += "\\b"; break;
		case '\f': result += "\\f"; break;
		case '\n': result += "\\n"; break;
		case '\r': result += "\\r"; break;
		case '\t': result += "\\t"; break;
		default:
			if (character < 0x20u) {
				std::ostringstream escaped;
				escaped << "\\u" << std::hex << std::setw(4)
					<< std::setfill('0') << static_cast<unsigned>(character);
				result += escaped.str();
			} else {
				result.push_back(static_cast<char>(character));
			}
		}
	}
	result.push_back('"');
	return result;
}

std::string value_kind_name(mnc::meter::MeterAttributeValueKind kind)
{
	switch (kind) {
	case mnc::meter::MeterAttributeValueKind::Linear: return "linear";
	case mnc::meter::MeterAttributeValueKind::CircularAngle:
		return "circular_angle";
	case mnc::meter::MeterAttributeValueKind::CumulativeCounter:
		return "cumulative_counter";
	case mnc::meter::MeterAttributeValueKind::Peak: return "peak";
	case mnc::meter::MeterAttributeValueKind::Categorical:
		return "categorical";
	}
	return "unknown";
}

std::string sha256(std::string_view body)
{
	std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
	SHA256(reinterpret_cast<const unsigned char *>(body.data()), body.size(),
		digest.data());
	std::ostringstream result;
	result << std::hex << std::setfill('0');
	for (const auto byte : digest)
		result << std::setw(2) << static_cast<unsigned>(byte);
	return result.str();
}

GeneratedContent content(const GeneratedDataset &dataset, std::string body,
	std::string mime_type, std::string extension)
{
	GeneratedContent result;
	result.artifact_id = dataset.artifact_id;
	result.filename = dataset.artifact_id + "." + extension;
	result.mime_type = std::move(mime_type);
	result.extension = std::move(extension);
	result.body = std::move(body);
	result.sha256 = sha256(result.body);
	return result;
}

void csv_field(std::string &output, std::string_view value, bool &first)
{
	if (!first)
		output.push_back(',');
	first = false;
	const bool quote = value.find_first_of(",\"\r\n") != std::string_view::npos;
	if (!quote) {
		output.append(value);
		return;
	}
	output.push_back('"');
	for (const auto character : value) {
		if (character == '"')
			output.push_back('"');
		output.push_back(character);
	}
	output.push_back('"');
}

void csv_line_end(std::string &output) { output += "\r\n"; }

} // namespace

GeneratedContent JsonMeterDataContentWriter::write(
	const GeneratedDataset &dataset) const
{
	if (dataset.format != ContentFormat::Json)
		throw DatalogError(DatalogErrorCode::SerializationFailure,
			"JSON writer received a non-JSON dataset");
	std::ostringstream output;
	output << "{\n"
		<< "  \"schema\": " << json_string(dataset.schema) << ",\n"
		<< "  \"artifact_id\": " << json_string(dataset.artifact_id) << ",\n"
		<< "  \"job_id\": " << json_string(dataset.job_id) << ",\n"
		<< "  \"job_revision\": \"" << dataset.job_revision << "\",\n"
		<< "  \"product_id\": " << json_string(dataset.product_id) << ",\n"
		<< "  \"device_id\": " << json_string(dataset.device_id) << ",\n"
		<< "  \"format\": \"json\",\n"
		<< "  \"source_period\": "
		<< json_string(period_name(dataset.source_period)) << ",\n"
		<< "  \"generated_at_utc_ns\": \"" << dataset.generated_at
		<< "\",\n"
		<< "  \"artifact_window\": {\"start_utc_ns\": \""
		<< dataset.artifact_window.start << "\", \"end_utc_ns\": \""
		<< dataset.artifact_window.end << "\"},\n"
		<< "  \"columns\": [\n";
	for (std::size_t index = 0; index < dataset.columns.size(); ++index) {
		const auto &column = dataset.columns[index];
		output << "    {\"id\": " << json_string(column.id)
			<< ", \"attribute\": " << json_string(column.attribute_key)
			<< ", \"label\": " << json_string(column.label)
			<< ", \"unit\": " << json_string(column.unit)
			<< ", \"calculation\": "
			<< json_string(calculation_name(column.calculation))
			<< ", \"value_kind\": "
			<< json_string(value_kind_name(column.value_kind)) << "}"
			<< (index + 1 == dataset.columns.size() ? "\n" : ",\n");
	}
	output << "  ],\n  \"rows\": [\n";
	for (std::size_t row_index = 0; row_index < dataset.rows.size();
	     ++row_index) {
		const auto &row = dataset.rows[row_index];
		if (row.cells.size() != dataset.columns.size())
			throw DatalogError(DatalogErrorCode::SerializationFailure,
				"dataset row/column cardinality mismatch");
		output << "    {\"start_utc_ns\": \"" << row.window.start
			<< "\", \"end_utc_ns\": \"" << row.window.end
			<< "\", \"values\": [";
		for (std::size_t cell_index = 0; cell_index < row.cells.size();
		     ++cell_index) {
			const auto &cell = row.cells[cell_index];
			output << "{\"value\": ";
			if (cell.value)
				output << json_string(*cell.value);
			else
				output << "null";
			output << ", \"quality\": " << json_string(quality_name(cell.quality))
				<< ", \"contributing_samples\": \""
				<< cell.contributing_samples
				<< "\", \"expected_samples\": \""
				<< cell.expected_samples << "\", \"complete\": "
				<< (cell.complete ? "true" : "false")
				<< ", \"continuity\": "
				<< (cell.continuity ? "true" : "false")
				<< ", \"reset_epoch\": ";
			if (cell.reset_epoch)
				output << "\"" << *cell.reset_epoch << "\"";
			else
				output << "null";
			output << "}" <<
				(cell_index + 1 == row.cells.size() ? "" : ", ");
		}
		output << "]}" <<
			(row_index + 1 == dataset.rows.size() ? "\n" : ",\n");
	}
	output << "  ]\n}\n";
	return content(dataset, output.str(), "application/json", "json");
}

GeneratedContent CsvMeterDataContentWriter::write(
	const GeneratedDataset &dataset) const
{
	if (dataset.format != ContentFormat::Csv)
		throw DatalogError(DatalogErrorCode::SerializationFailure,
			"CSV writer received a non-CSV dataset");
	std::string output;
	bool first = true;
	for (const auto field : {"schema", "artifact_id", "job_id",
		"job_revision", "product_id", "device_id", "source_period",
		"generated_at_utc_ns", "artifact_start_utc_ns",
		"artifact_end_utc_ns", "row_start_utc_ns", "row_end_utc_ns"})
		csv_field(output, field, first);
	for (const auto &column : dataset.columns) {
		for (const auto suffix : {"value", "quality", "contributing_samples",
			"expected_samples", "complete", "continuity", "reset_epoch"})
			csv_field(output, column.id + "." + suffix, first);
	}
	csv_line_end(output);

	for (const auto &row : dataset.rows) {
		if (row.cells.size() != dataset.columns.size())
			throw DatalogError(DatalogErrorCode::SerializationFailure,
				"dataset row/column cardinality mismatch");
		first = true;
		csv_field(output, dataset.schema, first);
		csv_field(output, dataset.artifact_id, first);
		csv_field(output, dataset.job_id, first);
		csv_field(output, std::to_string(dataset.job_revision), first);
		csv_field(output, dataset.product_id, first);
		csv_field(output, dataset.device_id, first);
		csv_field(output, period_name(dataset.source_period), first);
		csv_field(output, std::to_string(dataset.generated_at), first);
		csv_field(output, std::to_string(dataset.artifact_window.start), first);
		csv_field(output, std::to_string(dataset.artifact_window.end), first);
		csv_field(output, std::to_string(row.window.start), first);
		csv_field(output, std::to_string(row.window.end), first);
		for (const auto &cell : row.cells) {
			csv_field(output, cell.value.value_or(""), first);
			csv_field(output, quality_name(cell.quality), first);
			csv_field(output, std::to_string(cell.contributing_samples), first);
			csv_field(output, std::to_string(cell.expected_samples), first);
			csv_field(output, cell.complete ? "true" : "false", first);
			csv_field(output, cell.continuity ? "true" : "false", first);
			csv_field(output, cell.reset_epoch
				? std::to_string(*cell.reset_epoch) : std::string{}, first);
		}
		csv_line_end(output);
	}
	return content(dataset, std::move(output),
		"text/csv; charset=utf-8", "csv");
}

std::unique_ptr<MeterDataContentWriter>
DefaultMeterDataContentWriterFactory::create(ContentFormat format) const
{
	switch (format) {
	case ContentFormat::Json:
		return std::make_unique<JsonMeterDataContentWriter>();
	case ContentFormat::Csv:
		return std::make_unique<CsvMeterDataContentWriter>();
	}
	throw DatalogError(DatalogErrorCode::InvalidConfiguration,
		"unknown datalog content format");
}

} // namespace mnc::datalogger
