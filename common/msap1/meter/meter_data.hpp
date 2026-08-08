#pragma once

#include "msap1/meter/meter_record.hpp"
#include "msap1/meter/meter_timing.hpp"

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

/* The timing vocabulary lives in msap1::meter (meter_timing.hpp); pull the
 * names used throughout the decoded-data model into this namespace. */
using meter::BlockTiming;
using meter::MeasurementPeriod;
using meter::NominalFrequency;
using meter::TimeQuality;

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
	MeasurementPeriod period = MeasurementPeriod::Basic;
	RecordKind kind = RecordKind::fundamental;
	std::uint64_t sequence = 0;
	std::uint32_t configuration_generation = 0;
	std::optional<FundamentalValues> fundamental;
	std::optional<PowerValues> power;
	std::optional<EnergyValues> energy;
	std::optional<DemandValues> demand;
	std::optional<PowerQualityValues> power_quality;
	/* Cycle-timing identity of the source block. Present for record
	 * format v2; absent for v1 records, which predate cycle timing.
	 * The Basic period has no fixed duration — the actual duration is
	 * sample_count / sample_rate per block (see SampleWindow). */
	std::optional<BlockTiming> timing;
};

struct MeterPeriodView {
	MeasurementPeriod period = MeasurementPeriod::Basic;
	std::uint64_t latest_sequence = 0;
	std::uint32_t configuration_generation = 0;
	SystemTime updated_at{};
	MeterValues values{};
	std::optional<BlockTiming> timing{};
};

class MeterLatestStore {
public:
	void apply(const MeterUpdate &update);

	[[nodiscard]] std::optional<MeterPeriodView>
	latest(MeasurementPeriod period) const;

private:
	static constexpr std::size_t period_count = 4;
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
	latest(MeasurementPeriod period) const;

	[[nodiscard]] Subscription subscribe(MeasurementPeriod period,
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

/** Decode a v1 (0x00010001) record: fundamental values, no block timing. */
[[nodiscard]] MeterUpdate decode_periodic_meter_record(
	const MeterRecord &record, SystemTime received_at =
					     std::chrono::system_clock::now());

/**
 * Decode a v2 (0x00010002) record: fundamental values plus the BlockTiming
 * identity from words 6/15/60/61. TimeQuality and utc_start are NOT in the
 * record — the PL does not know UTC state — so the decoder leaves them at
 * Unsynchronized/absent and the caller (the ingestor) stamps them from the
 * MeasurementTimebase after decoding.
 */
[[nodiscard]] MeterUpdate decode_periodic_meter_record_v2(
	const MeterRecord &record, SystemTime received_at =
					     std::chrono::system_clock::now());

} // namespace msap1
