#pragma once

#include "msap1/meter_record.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace msap1 {

enum class UpdatePeriod : std::uint8_t {
	ms200 = 0,
	s1,
	s3,
	s10,
	min10,
	h2,
};

enum class RecordKind : std::uint16_t {
	fundamental = 1,
	power = 2,
	energy = 3,
	demand = 4,
	power_quality = 5,
};

enum class MeasurementQuality : std::uint8_t {
	unavailable = 0,
	valid,
	invalid,
	out_of_range,
	timed_out,
	arithmetic_error,
};

struct MilliHertz {};
struct MicroVolts {};
struct MicroAmperes {};
struct MicroWatts {};
struct MicroWattHours {};

using SystemTime = std::chrono::system_clock::time_point;

struct SampleWindow {
	std::uint32_t sample_count = 0;
	std::chrono::nanoseconds duration{};
};

template<typename Unit>
struct Reading {
	std::int64_t value = 0;
	MeasurementQuality quality = MeasurementQuality::unavailable;
	std::uint64_t source_sequence = 0;
	SystemTime measured_at{};
	SampleWindow calculation_window{};

	[[nodiscard]] bool available() const noexcept
	{
		return quality != MeasurementQuality::unavailable;
	}
	[[nodiscard]] bool valid() const noexcept
	{
		return quality == MeasurementQuality::valid;
	}
};

template<typename T>
struct PhaseABC {
	T phase_a{};
	T phase_b{};
	T phase_c{};
};

template<typename T>
struct PhaseABCN {
	T phase_a{};
	T phase_b{};
	T phase_c{};
	T neutral{};
};

struct FundamentalValues {
	Reading<MilliHertz> frequency{};
	PhaseABC<Reading<MicroVolts>> voltage_ln{};
	PhaseABCN<Reading<MicroAmperes>> current{};
};

/* These typed groups intentionally start empty. New PL record decoders add
 * concrete readings here without changing the transport or period store. */
struct PowerValues {};
struct EnergyValues {};
struct DemandValues {};
struct PowerQualityValues {};

struct MeterValues {
	FundamentalValues fundamental{};
	PowerValues power{};
	EnergyValues energy{};
	DemandValues demand{};
	PowerQualityValues power_quality{};
};

struct MeterUpdate {
	UpdatePeriod period = UpdatePeriod::ms200;
	RecordKind kind = RecordKind::fundamental;
	std::uint64_t sequence = 0;
	std::uint32_t configuration_generation = 0;
	std::optional<FundamentalValues> fundamental;
	std::optional<PowerValues> power;
	std::optional<EnergyValues> energy;
	std::optional<DemandValues> demand;
	std::optional<PowerQualityValues> power_quality;
};

struct MeterPeriodView {
	UpdatePeriod period = UpdatePeriod::ms200;
	std::uint64_t latest_sequence = 0;
	std::uint32_t configuration_generation = 0;
	SystemTime updated_at{};
	MeterValues values{};
};

[[nodiscard]] std::chrono::milliseconds duration(UpdatePeriod period);
[[nodiscard]] std::optional<UpdatePeriod>
update_period(std::chrono::milliseconds duration);

class MeterLatestStore {
public:
	void apply(const MeterUpdate &update);

	[[nodiscard]] std::optional<MeterPeriodView>
	latest(UpdatePeriod period) const;

private:
	static constexpr std::size_t period_count = 6;
	mutable std::mutex mutex_;
	std::array<std::optional<MeterPeriodView>, period_count> views_{};
};

class MeterData {
public:
	using UpdateCallback = std::function<void(const MeterPeriodView &)>;

	class Subscription {
	public:
		Subscription() = default;
		Subscription(const Subscription &) = delete;
		Subscription &operator=(const Subscription &) = delete;
		Subscription(Subscription &&other) noexcept;
		Subscription &operator=(Subscription &&other) noexcept;
		~Subscription();

	private:
		friend class MeterData;
		Subscription(std::weak_ptr<void> state, std::uint64_t id);
		std::weak_ptr<void> state_;
		std::uint64_t id_ = 0;
	};

	MeterData();
	void apply(const MeterUpdate &update);

	[[nodiscard]] std::optional<MeterPeriodView>
	latest(UpdatePeriod period) const;

	[[nodiscard]] Subscription subscribe(UpdatePeriod period,
					     UpdateCallback callback);

private:
	struct State;
	std::shared_ptr<State> state_;
};

/**
 * Extensible decoder table keyed by the PL record format/version word.
 * Future power, energy, demand, and PQ records can register new decoders
 * without modifying the existing MTR1 fundamental decoder.
 */
class MeterDecoderRegistry {
public:
	using Decoder = std::function<MeterUpdate(const MeterRecord &, SystemTime)>;

	void register_decoder(std::uint32_t record_format, Decoder decoder);
	[[nodiscard]] MeterUpdate decode(
		const MeterRecord &record,
		SystemTime received_at = std::chrono::system_clock::now()) const;

	[[nodiscard]] static MeterDecoderRegistry with_builtin_decoders();

private:
	std::map<std::uint32_t, Decoder> decoders_;
};

[[nodiscard]] MeterUpdate decode_periodic_meter_record(
	const MeterRecord &record, SystemTime received_at =
					     std::chrono::system_clock::now());

} // namespace msap1
