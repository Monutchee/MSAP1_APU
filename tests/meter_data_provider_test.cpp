#include "msap1/meter/MeterDataProvider/msap1_meter_data_provider.hpp"

#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

msap1::Reading<msap1::MicroVolts> voltage(
	std::int64_t value, msap1::MeasurementQuality quality,
	std::uint64_t sequence)
{
	return {value, quality, sequence, std::chrono::system_clock::now(),
		{6400, std::chrono::milliseconds(200)}};
}

void projects_selected_values_and_unavailable_attributes()
{
	using mnc::meter::MeterAttributeId;
	using mnc::meter::MeterAttributeKey;
	using mnc::meter::ReadingQuality;

	msap1::MeterData data;
	msap1::MeterUpdate update{};
	update.period = msap1::MeasurementPeriod::Basic;
	update.sequence = 42;
	update.configuration_generation = 7;
	update.fundamental.emplace();
	update.fundamental->voltage_ln.phase_a =
		voltage(120'000'000, msap1::MeasurementQuality::valid, 42);
	/* Valid electrical zero must remain distinct from unavailable. */
	update.fundamental->voltage_ln.phase_b =
		voltage(0, msap1::MeasurementQuality::valid, 42);
	data.apply(update);

	msap1::meter::Msap1MeterDataProvider provider(data);
	mnc::meter::MeterSnapshotRequest request{};
	request.attributes = {
		MeterAttributeKey{MeterAttributeId::VanRms, std::nullopt},
		MeterAttributeKey{MeterAttributeId::VbnRms, std::nullopt},
		MeterAttributeKey{MeterAttributeId::VabRms, std::nullopt},
		MeterAttributeKey{MeterAttributeId::VanRms, 5},
	};
	const auto snapshot = provider.latest(request);
	require(snapshot.has_value(), "provider lost the current snapshot");
	require(snapshot->sequence == 42 && snapshot->values.size() == 4,
		"provider returned the wrong snapshot identity or selection");
	require(snapshot->values[0].quality == ReadingQuality::Valid &&
		snapshot->values[0].value == 120'000'000,
		"phase-A voltage was not projected");
	require(snapshot->values[1].quality == ReadingQuality::Valid &&
		snapshot->values[1].value == 0,
		"valid zero was confused with unavailable data");
	require(snapshot->values[2].quality == ReadingQuality::Unavailable,
		"unsupported line-line voltage was not explicit");
	require(snapshot->values[3].quality == ReadingQuality::Unavailable,
		"future indexed attribute aliased the fundamental reading");
}

void empty_selection_means_all_supported_values()
{
	msap1::MeterData data;
	msap1::MeterUpdate update{};
	update.period = msap1::MeasurementPeriod::Cycles150_180;
	update.sequence = 5;
	update.fundamental.emplace();
	data.apply(update);
	msap1::meter::Msap1MeterDataProvider provider(data);
	mnc::meter::MeterSnapshotRequest request{};
	request.period = mnc::meter::MeasurementPeriod::Cycles150_180;
	const auto snapshot = provider.latest(request);
	require(snapshot.has_value() && snapshot->values.size() == 7,
		"empty selection did not expand to period capabilities");
}

} // namespace

int main()
{
	try {
		projects_selected_values_and_unavailable_attributes();
		empty_selection_means_all_supported_values();
		std::cout << "meter data provider tests passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "meter data provider test failed: " << error.what()
			  << '\n';
		return 1;
	}
}
