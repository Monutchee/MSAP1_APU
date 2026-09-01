#pragma once

#include "mnc/datalogger/types.hpp"

#include <memory>

namespace mnc::datalogger {

struct GeneratedContent {
	std::string artifact_id;
	std::string filename;
	std::string mime_type;
	std::string extension;
	std::string body;
	std::string sha256;
};

class MeterDataContentWriter {
public:
	virtual ~MeterDataContentWriter() = default;
	[[nodiscard]] virtual ContentFormat format() const noexcept = 0;
	[[nodiscard]] virtual GeneratedContent write(
		const GeneratedDataset &dataset) const = 0;
};

class MeterDataContentWriterFactory {
public:
	virtual ~MeterDataContentWriterFactory() = default;
	[[nodiscard]] virtual std::unique_ptr<MeterDataContentWriter> create(
		ContentFormat format) const = 0;
};

class JsonMeterDataContentWriter final : public MeterDataContentWriter {
public:
	[[nodiscard]] ContentFormat format() const noexcept override
	{
		return ContentFormat::Json;
	}
	[[nodiscard]] GeneratedContent write(
		const GeneratedDataset &dataset) const override;
};

class CsvMeterDataContentWriter final : public MeterDataContentWriter {
public:
	[[nodiscard]] ContentFormat format() const noexcept override
	{
		return ContentFormat::Csv;
	}
	[[nodiscard]] GeneratedContent write(
		const GeneratedDataset &dataset) const override;
};

class DefaultMeterDataContentWriterFactory final
	: public MeterDataContentWriterFactory {
public:
	[[nodiscard]] std::unique_ptr<MeterDataContentWriter> create(
		ContentFormat format) const override;
};

} // namespace mnc::datalogger
