#include "mnc/MeterDataProvider/meter_data_provider.hpp"
#include "msap1/meter/MeterDataProvider/snapshot/in_process_meter_snapshot_provider.hpp"

#include <chrono>
#include <condition_variable>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace {

class FakeMeterStreamConsumer final
	: public mnc::meter_stream::MeterStreamConsumer {
public:
	void register_consumer(std::string_view) override {}
	void unregister_consumer(std::string_view) override {}
	std::vector<mnc::meter_stream::MeterStreamRecord> read_after(
		std::string_view, std::size_t) override
	{
		return {};
	}
	void acknowledge(std::string_view, std::uint64_t) override {}
};

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

void describes_protocol_independent_attributes()
{
	using mnc::meter::MeterAttributeId;
	using mnc::meter::MeterAttributeKey;
	using mnc::meter::MeterUnit;

	const auto voltage = mnc::meter::describe(
		MeterAttributeKey{MeterAttributeId::VanRms, std::nullopt});
	const auto frequency = mnc::meter::describe(
		MeterAttributeKey{MeterAttributeId::Frequency, std::nullopt});
	require(voltage.key == "voltage.ln.a.rms" &&
			voltage.unit == MeterUnit::MicroVolts,
		"phase-A voltage descriptor changed");
	require(frequency.key == "frequency" &&
			frequency.unit == MeterUnit::MilliHertz,
		"frequency descriptor changed");

	const auto defined = mnc::meter::attributes_in(
		mnc::meter::MeterAttributeGroup::AllDefined);
	require(defined.size() == 87 &&
			defined.front().id == MeterAttributeId::Frequency,
		"AllDefined no longer describes the canonical catalog");
	const auto quadrant = mnc::meter::describe(MeterAttributeKey{
		MeterAttributeId::ReactiveEnergyQuadrantIIITotal, std::nullopt});
	require(quadrant.key == "energy.reactive.quadrant_iii.total" &&
			quadrant.unit == MeterUnit::MicroVarHours,
		"quadrant-III energy descriptor changed");
}

void capabilities_advertise_only_supported_periods()
{
	msap1::MeterData data;
	msap1::meter::InProcessMeterSnapshotProvider provider(data);
	const auto capabilities = provider.capabilities();
	require(capabilities.size() == 7,
		"provider did not advertise every implemented completed/live period");
	require(capabilities[0].period ==
			mnc::meter::MeasurementPeriod::Basic &&
			!capabilities[0].attributes.empty(),
		"basic period capability is missing");
	require(capabilities[1].period ==
			mnc::meter::MeasurementPeriod::Cycles150_180 &&
			!capabilities[1].attributes.empty(),
		"150/180-cycle capability is missing");
	require(capabilities[2].period ==
			mnc::meter::MeasurementPeriod::Min10 &&
			!capabilities[2].attributes.empty(),
		"ten-minute capability is missing");
	require(capabilities[3].period ==
			mnc::meter::MeasurementPeriod::Hour2 &&
			!capabilities[3].attributes.empty(),
		"two-hour capability is missing");
	require(capabilities[4].period ==
			mnc::meter::MeasurementPeriod::Min10Live &&
			!capabilities[4].attributes.empty(),
		"ten-minute live-partial capability is missing");
	require(capabilities[5].period ==
			mnc::meter::MeasurementPeriod::Hour2Live &&
			!capabilities[5].attributes.empty(),
		"two-hour live-partial capability is missing");
	require(capabilities[6].period ==
			mnc::meter::MeasurementPeriod::Demand &&
			!capabilities[6].attributes.empty(),
		"configured demand capability is missing");
}

void facade_exposes_the_injected_delivery_paths()
{
	msap1::MeterData data;
	msap1::meter::InProcessMeterSnapshotProvider snapshots(data);
	FakeMeterStreamConsumer stream;
	mnc::meter::MeterDataProviderView view(snapshots, stream);
	mnc::meter::MeterDataProvider &provider = view;

	require(&provider.snapshot_provider() == &snapshots,
		"MeterDataProvider changed the injected snapshot provider");
	require(&provider.stream_consumer() == &stream,
		"MeterDataProvider changed the injected stream consumer");

	const mnc::meter::MeterDataProvider &const_provider = view;
	require(&const_provider.snapshot_provider() == &snapshots,
		"const MeterDataProvider changed the snapshot provider");
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

	msap1::meter::InProcessMeterSnapshotProvider provider(data);
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

void preserves_explicit_order_after_deduplication()
{
	using mnc::meter::MeterAttributeId;
	using mnc::meter::MeterAttributeKey;

	msap1::MeterData data;
	msap1::MeterUpdate update{};
	update.period = msap1::MeasurementPeriod::Basic;
	update.sequence = 9;
	update.fundamental.emplace();
	data.apply(update);

	msap1::meter::InProcessMeterSnapshotProvider provider(data);
	mnc::meter::MeterSnapshotRequest request{};
	request.attributes = {
		MeterAttributeKey{MeterAttributeId::VanRms, std::nullopt},
		MeterAttributeKey{MeterAttributeId::IaRms, std::nullopt},
		MeterAttributeKey{MeterAttributeId::VanRms, std::nullopt},
		MeterAttributeKey{MeterAttributeId::Frequency, std::nullopt},
	};
	const auto snapshot = provider.latest(request);
	require(snapshot && snapshot->values.size() == 3,
		"explicit selection was not deduplicated");
	require(snapshot->values[0].attribute.id == MeterAttributeId::VanRms &&
			snapshot->values[1].attribute.id == MeterAttributeId::IaRms &&
			snapshot->values[2].attribute.id ==
				MeterAttributeId::Frequency,
		"explicit selection order was not preserved");
}

void rejects_unknown_attribute_identity()
{
	msap1::MeterData data;
	msap1::MeterUpdate update{};
	update.period = msap1::MeasurementPeriod::Basic;
	update.sequence = 1;
	update.fundamental.emplace();
	data.apply(update);

	msap1::meter::InProcessMeterSnapshotProvider provider(data);
	mnc::meter::MeterSnapshotRequest request{};
	request.attributes = {{
		static_cast<mnc::meter::MeterAttributeId>(0xffffu), std::nullopt}};
	bool rejected = false;
	try {
		(void)provider.latest(request);
	} catch (const std::invalid_argument &) {
		rejected = true;
	}
	require(rejected, "unknown attribute identity was silently ignored");
}

void empty_selection_means_all_supported_values()
{
	msap1::MeterData data;
	msap1::MeterUpdate update{};
	update.period = msap1::MeasurementPeriod::Cycles150_180;
	update.sequence = 5;
	update.fundamental.emplace();
	data.apply(update);
	msap1::meter::InProcessMeterSnapshotProvider provider(data);
	mnc::meter::MeterSnapshotRequest request{};
	request.period = mnc::meter::MeasurementPeriod::Cycles150_180;
	const auto snapshot = provider.latest(request);
	/* M11: the aggregate period carries the full quantity set (7 RMS
	 * lanes + VLL + power + phasor + unbalance = 46; frequency stays
	 * basic-only). */
	require(snapshot.has_value() && snapshot->values.size() == 46,
		"empty selection did not expand to period capabilities");
}

msap1::BlockTiming block_timing(msap1::TimeQuality quality)
{
	msap1::BlockTiming timing{};
	timing.sequence = 77;
	timing.configuration_generation = 9;
	timing.first_sample_index = 123'456;
	timing.sample_count = 7'680;
	timing.cycle_count = 12;
	timing.nominal_frequency = msap1::NominalFrequency::Hz60;
	timing.cycle_locked = true;
	timing.time_quality = quality;
	timing.utc_start = std::chrono::system_clock::time_point{
		std::chrono::nanoseconds{1'700'000'000'000'000'000ll}};
	timing.utc_uncertainty_ns = 250;
	return timing;
}

void preserves_measurement_time_provenance()
{
	using mnc::meter::MeterAttributeId;
	using mnc::meter::MeterAttributeKey;
	using mnc::meter::TimeQuality;

	msap1::MeterData data;
	msap1::MeterUpdate update{};
	update.period = msap1::MeasurementPeriod::Basic;
	update.sequence = 77;
	update.configuration_generation = 9;
	update.fundamental.emplace();
	update.fundamental->voltage_ln.phase_a =
		voltage(120'000'000, msap1::MeasurementQuality::valid, 77);
	update.timing = block_timing(msap1::TimeQuality::Synchronized);
	data.apply(update);

	msap1::meter::InProcessMeterSnapshotProvider provider(data);
	mnc::meter::MeterSnapshotRequest request{};
	request.attributes = {{MeterAttributeId::VanRms, std::nullopt}};
	const auto latest = provider.latest(request);
	require(latest && latest->timing, "snapshot timing was not projected");
	require(latest->timing->quality == TimeQuality::Synchronized &&
			latest->timing->first_sample_index == 123'456 &&
			latest->timing->sample_count == 7'680 &&
			latest->timing->cycle_count == 12 &&
			latest->timing->nominal_frequency_hz == 60 &&
			latest->timing->utc_start_nanoseconds.has_value() &&
			latest->timing->utc_uncertainty_nanoseconds == 250,
		"measurement timing provenance was changed during projection");

	/* Time quality is independent of electrical quality, including a valid
	 * zero reading. */
	require(latest->values.front().quality ==
			mnc::meter::ReadingQuality::Valid,
		"electrical quality was coupled to time quality");

	/* A later holdover state belongs to future ingested blocks; it must not
	 * rewrite this already-published synchronized snapshot. */
	update.sequence = 78;
	update.timing = block_timing(msap1::TimeQuality::Holdover);
	update.timing->utc_start.reset();
	update.timing->utc_uncertainty_ns.reset();
	data.apply(update);
	const auto holdover = provider.latest(request);
	require(holdover && holdover->timing &&
		holdover->timing->quality == TimeQuality::Holdover &&
		!holdover->timing->utc_start_nanoseconds,
		"holdover provenance was not retained for the new block");
}

void latest_and_subscription_share_timing_projection()
{
	msap1::MeterData data;
	msap1::MeterUpdate update{};
	update.period = msap1::MeasurementPeriod::Basic;
	update.sequence = 11;
	update.fundamental.emplace();
	update.timing = block_timing(msap1::TimeQuality::Unsynchronized);
	data.apply(update);

	msap1::meter::InProcessMeterSnapshotProvider provider(data);
	mnc::meter::MeterSnapshotRequest request{};
	request.attributes = {{mnc::meter::MeterAttributeId::Frequency,
		std::nullopt}};
	const auto expected = provider.latest(request);
	require(expected && expected->timing &&
		expected->timing->quality ==
			mnc::meter::TimeQuality::Unsynchronized &&
		expected->timing->first_sample_index == 123'456,
		"unsynchronized sample-domain timing was lost");

	std::mutex mutex;
	std::condition_variable condition;
	std::optional<mnc::meter::MeterSnapshot> received;
	const auto subscription = provider.subscribe_latest(
		request, [&](const mnc::meter::MeterSnapshot &snapshot) {
			{
				std::scoped_lock lock(mutex);
				received = snapshot;
			}
			condition.notify_one();
		});
	(void)subscription;
	update.sequence = 12;
	data.apply(update);
	{
		std::unique_lock lock(mutex);
		require(condition.wait_for(lock, std::chrono::seconds(1), [&] {
			return received.has_value();
		}), "latest subscription did not receive the update");
	}
	require(received->timing && expected->timing &&
		received->timing->quality == expected->timing->quality &&
		received->timing->first_sample_index ==
			expected->timing->first_sample_index,
		"latest and subscription timing projections differ");
}

void subscription_does_not_capture_provider_lifetime()
{
	msap1::MeterData data;
	mnc::meter::MeterSnapshotRequest request{};
	request.attributes = {{mnc::meter::MeterAttributeId::VanRms,
		std::nullopt}};
	std::mutex mutex;
	std::condition_variable condition;
	bool received = false;
	mnc::meter::LatestSubscription subscription;
	{
		auto provider = std::make_unique<
			msap1::meter::InProcessMeterSnapshotProvider>(data);
		subscription = provider->subscribe_latest(
			request, [&](const mnc::meter::MeterSnapshot &) {
				std::scoped_lock lock(mutex);
				received = true;
				condition.notify_one();
			});
	} /* provider is gone; MeterData owns the subscription worker. */
	msap1::MeterUpdate update{};
	update.period = msap1::MeasurementPeriod::Basic;
	update.sequence = 1;
	update.fundamental.emplace();
	data.apply(update);
	std::unique_lock lock(mutex);
	require(condition.wait_for(lock, std::chrono::seconds(1), [&] {
		return received;
	}), "subscription retained a dangling provider reference");
}

} // namespace

int main()
{
	try {
		describes_protocol_independent_attributes();
		capabilities_advertise_only_supported_periods();
		facade_exposes_the_injected_delivery_paths();
		projects_selected_values_and_unavailable_attributes();
		preserves_explicit_order_after_deduplication();
		rejects_unknown_attribute_identity();
		empty_selection_means_all_supported_values();
		preserves_measurement_time_provenance();
		latest_and_subscription_share_timing_projection();
		subscription_does_not_capture_provider_lifetime();
		std::cout << "meter data provider tests passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "meter data provider test failed: " << error.what()
			  << '\n';
		return 1;
	}
}
