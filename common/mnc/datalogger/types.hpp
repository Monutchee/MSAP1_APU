#pragma once

#include "mnc/MeterDataProvider/attributes/meter_attribute.hpp"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace mnc::datalogger {

using UtcNanoseconds = std::int64_t;

struct UtcWindow {
	UtcNanoseconds start = 0;
	UtcNanoseconds end = 0;

	[[nodiscard]] constexpr bool valid() const noexcept { return start < end; }
	[[nodiscard]] constexpr std::int64_t duration_nanoseconds() const noexcept
	{
		return end - start;
	}
	auto operator<=>(const UtcWindow &) const = default;
};

enum class ContentFormat : std::uint8_t { Json, Csv };

struct AttributeSelection {
	mnc::meter::MeterAttributeKey attribute;
	mnc::meter::MeterAttributeCalculation calculation =
		mnc::meter::MeterAttributeCalculation::Last;
	auto operator<=>(const AttributeSelection &) const = default;
};

struct DatalogJobSnapshot {
	std::string job_id;
	std::uint64_t revision = 1;
	std::string product_id;
	std::string device_id;
	mnc::meter::MeasurementPeriod source_period =
		mnc::meter::MeasurementPeriod::Basic;
	std::int64_t generation_interval_nanoseconds = 0;
	std::int64_t row_interval_nanoseconds = 0;
	ContentFormat format = ContentFormat::Json;
	std::vector<AttributeSelection> selections;
};

struct HistoricalSample {
	UtcNanoseconds measured_at = 0;
	std::uint64_t source_sequence = 0;
	mnc::meter::MeterAttributeKey attribute;
	std::int64_t value = 0;
	mnc::meter::ReadingQuality quality =
		mnc::meter::ReadingQuality::Unavailable;
	std::optional<std::uint64_t> reset_epoch;
};

struct DatasetColumn {
	std::string id;
	std::string attribute_key;
	std::string label;
	std::string unit;
	mnc::meter::MeterAttributeCalculation calculation =
		mnc::meter::MeterAttributeCalculation::Last;
	mnc::meter::MeterAttributeValueKind value_kind =
		mnc::meter::MeterAttributeValueKind::Linear;
};

struct DatasetCell {
	std::optional<std::string> value;
	mnc::meter::ReadingQuality quality =
		mnc::meter::ReadingQuality::Unavailable;
	std::uint64_t contributing_samples = 0;
	std::uint64_t expected_samples = 0;
	bool complete = false;
	bool continuity = true;
	std::optional<std::uint64_t> reset_epoch;
};

struct DatasetRow {
	UtcWindow window;
	std::vector<DatasetCell> cells;
};

struct GeneratedDataset {
	std::string schema = "mnc.meter.datalog.v1";
	std::string artifact_id;
	std::string job_id;
	std::uint64_t job_revision = 0;
	std::string product_id;
	std::string device_id;
	mnc::meter::MeasurementPeriod source_period =
		mnc::meter::MeasurementPeriod::Basic;
	ContentFormat format = ContentFormat::Json;
	UtcNanoseconds generated_at = 0;
	UtcWindow artifact_window;
	std::vector<DatasetColumn> columns;
	std::vector<DatasetRow> rows;
};

enum class DatalogErrorCode : std::uint8_t {
	InvalidConfiguration,
	UnsupportedCapability,
	SourceUnavailable,
	SourceRetentionGap,
	SerializationFailure,
	StorageFailure,
};

class DatalogError final : public std::runtime_error {
public:
	DatalogError(DatalogErrorCode code, std::string message)
		: std::runtime_error(std::move(message)), code_(code)
	{
	}

	[[nodiscard]] DatalogErrorCode code() const noexcept { return code_; }

private:
	DatalogErrorCode code_;
};

[[nodiscard]] std::string_view content_format_name(ContentFormat format) noexcept;
[[nodiscard]] std::string_view calculation_name(
	mnc::meter::MeterAttributeCalculation calculation) noexcept;
[[nodiscard]] std::string_view quality_name(
	mnc::meter::ReadingQuality quality) noexcept;
[[nodiscard]] std::string_view period_name(
	mnc::meter::MeasurementPeriod period) noexcept;

} // namespace mnc::datalogger
