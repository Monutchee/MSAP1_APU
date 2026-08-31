#pragma once

#include "mnc/datalogger/datalogger.hpp"
#include "msap1/meter/history/historian_ipc.hpp"

namespace msap1::datalogger {

class Msap1HistorianDataSource final
	: public mnc::datalogger::HistoricalMeterDataSource {
public:
	explicit Msap1HistorianDataSource(
		const msap1::history::ipc::HistorianClient &historian) noexcept
		: historian_(historian)
	{
	}

	[[nodiscard]] std::vector<mnc::datalogger::HistoricalSample> query(
		mnc::meter::MeasurementPeriod period,
		std::span<const mnc::meter::MeterAttributeKey> attributes,
		mnc::datalogger::UtcWindow window) const override;

	[[nodiscard]] std::uint64_t expected_sample_count(
		mnc::meter::MeasurementPeriod period,
		mnc::meter::MeterAttributeKey attribute,
		mnc::datalogger::UtcWindow window) const override;

private:
	const msap1::history::ipc::HistorianClient &historian_;
};

/** MSAP1 implementation of the reusable product-neutral Datalogger contract. */
class Msap1Datalogger final : public mnc::datalogger::Datalogger {
public:
	explicit Msap1Datalogger(
		const msap1::history::ipc::HistorianClient &historian) noexcept
		: source_(historian), generator_(source_)
	{
	}

	[[nodiscard]] mnc::datalogger::GeneratedDataset generate(
		const mnc::datalogger::DatalogJobSnapshot &job,
		mnc::datalogger::UtcWindow completed_window,
		mnc::datalogger::UtcNanoseconds generated_at) const override;

private:
	Msap1HistorianDataSource source_;
	mnc::datalogger::HistoricalDatasetGenerator generator_;
};

} // namespace msap1::datalogger
