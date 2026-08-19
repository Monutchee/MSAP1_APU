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
using meter::AggregateTiming;
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
	/* BASIC-v4 (M7): Vab/Vbc/Vca merged from the single-cycle tier's
	 * instantaneous difference statistics (never |Va|-|Vb|). Valid only
	 * when both contributing lanes are; default-invalid on aggregate
	 * (MTR2) updates until that tier carries VLL (M11). */
	PhaseABC<Reading<MicroVolts>> voltage_ll{};
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
	/* Cycle-timing identity of the source block. Present for every
	 * periodic (MTR1) update; absent for aggregate updates.
	 * The Basic period has no fixed duration — the actual duration is
	 * sample_count / sample_rate per block (see SampleWindow). */
	std::optional<BlockTiming> timing;
	/* Aggregation identity of the source record. Present exactly for
	 * 150/180-cycle aggregate (MTR2) records; basic updates leave it
	 * absent, and aggregate updates leave `timing` absent. */
	std::optional<AggregateTiming> aggregate_timing;
};

struct MeterPeriodView {
	MeasurementPeriod period = MeasurementPeriod::Basic;
	std::uint64_t latest_sequence = 0;
	std::uint32_t configuration_generation = 0;
	SystemTime updated_at{};
	MeterValues values{};
	std::optional<BlockTiming> timing{};
	std::optional<AggregateTiming> aggregate_timing{};
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
/*
 * Decoded view of one SCYC-v2 single-cycle diagnostic record (PL
 * metrology roadmap M3). Deliberately OUTSIDE the MeterUpdate/period
 * store: these are diagnostic readings for verification tooling; the
 * authoritative single-cycle outputs are the mergeable statistics on the
 * PL-internal result stream. All RMS values are micro-units (uV/uA).
 */
struct SingleCycleSnapshot {
	std::uint32_t sequence = 0;
	std::uint32_t configuration_generation = 0;
	std::uint32_t sample_count = 0;
	std::uint32_t valid_mask = 0;
	std::uint32_t status = 0;
	std::uint64_t first_sample = 0;
	std::uint64_t last_sample = 0;
	std::uint32_t cycle_sequence = 0;
	std::uint32_t nominal_hz = 0;
	std::uint32_t flags = 0;
	std::uint64_t processing_tick = 0;
	std::uint32_t frequency_millihz = 0;
	std::uint32_t frequency_status = 0;
	std::array<std::uint64_t, 7> rms_micro_units{};
	std::array<std::uint64_t, 3> vll_rms_micro_units{};
	/* Per-phase one-cycle active power, picowatts; import positive
	 * (sign conventions: PL metering_types.hpp). */
	std::array<std::int64_t, 3> active_power_picowatts{};
	/* Fundamental (phasor-magnitude) RMS per lane, micro-units.
	 * Meaningful only while phasor_valid(): the engine zeroes the
	 * section and sets status bit 1 when its frequency reference was
	 * invalid at the cycle start. */
	std::array<std::uint64_t, 7> fundamental_rms_micro_units{};
	[[nodiscard]] bool phasor_valid() const { return (status & 0x2u) == 0u; }
	/* SCYC-v5 status bits 2..4: every result is a whole cycle; the first
	 * one emitted after a discontinuity (reset, APPLY, malformed input,
	 * dropped beat, timing loss) is marked, with its cause. */
	[[nodiscard]] bool first_after_gap() const { return (status & 0x4u) != 0u; }
	[[nodiscard]] bool gap_was_malformed() const { return (status & 0x8u) != 0u; }
	[[nodiscard]] bool gap_was_timing() const { return (status & 0x10u) != 0u; }
};

[[nodiscard]] SingleCycleSnapshot decode_single_cycle_record(
	const MeterRecord &record);

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

/**
 * Decode an MTR1 (0x00010003) record: fundamental values plus the
 * BlockTiming identity from envelope words 6/9/10 and the timing word 13.
 * TimeQuality and utc_start are NOT in the record — the PL does not know
 * UTC state — so the decoder leaves them at Unsynchronized/absent and the
 * caller (the ingestor) stamps them from the MeasurementTimebase after
 * decoding.
 */
[[nodiscard]] MeterUpdate decode_periodic_meter_record(
	const MeterRecord &record, SystemTime received_at =
					     std::chrono::system_clock::now());

/**
 * Decode an MTR2 aggregate (0x00020002) record: 150/180-cycle fundamental
 * values plus the AggregateTiming identity. The PL is the authoritative
 * aggregator — this only DECODES what the PL computed; the APU never
 * recomputes aggregate values. As for MTR1, TimeQuality and utc_start are
 * not in the record: the caller (the ingestor) stamps them from the
 * MeasurementTimebase after decoding.
 */
[[nodiscard]] MeterUpdate decode_aggregate_meter_record(
	const MeterRecord &record, SystemTime received_at =
					     std::chrono::system_clock::now());

} // namespace msap1
