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
	phasor = 6,
	unbalance = 7,
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
struct Picowatts {};
struct PicoVoltAmperes {};
/* True power factor, millionths, sign follows P; 0 when S is 0 (PF
 * undefined -- gate on S). */
struct PowerFactorMillionths {};
/* Crest factor, ten-thousandths; 0 when the lane RMS is 0. */
struct CrestTenThousandths {};
/* Fundamental reactive power Q1, picovars; lagging/inductive positive
 * (sign conventions: PL metering_types.hpp). */
struct Picovars {};
/* Phase angle in millidegrees, [0, 360000) — the industry convention,
 * published this way by the PL itself (PHASOR/UNBAL v2) — relative to
 * the Va fundamental (Va reads exactly 0; a 120-degree lag reads
 * 240000). Meaningless when the record's angle-reference flag is clear
 * or the lane's fundamental is zero. */
struct Millidegrees {};
/* Unsigned ratio in millionths of the positive-sequence magnitude
 * (20000 = 2%); undefined (Unavailable quality) when |X1| = 0, clamped
 * at the u32 rail (an ACB feed drives it off scale by design). */
struct RatioMillionths {};
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
	/* Vab/Vbc/Vca merged from the single-cycle tier's instantaneous
	 * difference statistics (never |Va|-|Vb|). Valid only when both
	 * contributing lanes are. Basic since M7 (BASIC-v4 words 51..53);
	 * aggregate since M11 (AGG-v3 words 38..40). */
	PhaseABC<Reading<MicroVolts>> voltage_ll{};
};

/* Decoded from the POWER-v1 record (M8): the 10/12-cycle tier's
 * finalized power quantities, sign conventions normative in the PL's
 * metering_types.hpp (P import positive; S arithmetic, unsigned; PF true
 * P/S with P's sign, never averaged across phases). */
struct PowerValues {
	PhaseABC<Reading<Picowatts>> active_power{};
	PhaseABC<Reading<PicoVoltAmperes>> apparent_power{};
	PhaseABC<Reading<PowerFactorMillionths>> power_factor{};
	Reading<Picowatts> total_active_power{};
	Reading<PicoVoltAmperes> total_apparent_power{};
	Reading<PowerFactorMillionths> total_power_factor{};
	PhaseABC<Reading<CrestTenThousandths>> voltage_crest{};
	PhaseABCN<Reading<CrestTenThousandths>> current_crest{};
};
/* Load nature classified from Q1's sign under the S1 = 0 gate (the PL's
 * MET_NATURE_* codes, never inferred from a PF magnitude). */
enum class LoadNature : std::uint8_t {
	undefined = 0, /* S1 = 0 — nothing to classify */
	unity = 1,     /* Q1 = 0 exactly */
	lagging = 2,   /* Q1 > 0, inductive */
	leading = 3,   /* Q1 < 0, capacitive */
};

/* Decoded from the PHASOR-v1 record (M9): the 10/12-cycle tier's
 * fundamental quantities from the synchronous correlation. True PF (in
 * PowerValues) and displacement PF here are DISTINCT and diverge under
 * distortion. Angles are relative to Va; only differences are specified.
 * When the block carried the phasor-invalid status bit every reading here
 * decodes with MeasurementQuality::invalid. */
struct PhasorValues {
	PhaseABC<Reading<MicroVolts>> fundamental_voltage{};
	PhaseABCN<Reading<MicroAmperes>> fundamental_current{};
	PhaseABC<Reading<MicroVolts>> fundamental_voltage_ll{};
	PhaseABC<Reading<Millidegrees>> voltage_angle{};
	PhaseABCN<Reading<Millidegrees>> current_angle{};
	PhaseABC<Reading<Millidegrees>> voltage_ll_angle{};
	/* phi1 = angle(V1) - angle(I1); positive = current lags. */
	PhaseABC<Reading<Millidegrees>> displacement_angle{};
	PhaseABC<Reading<Picovars>> reactive_power{};
	Reading<Picovars> total_reactive_power{};
	PhaseABC<Reading<Picowatts>> fundamental_active_power{};
	Reading<Picowatts> total_fundamental_active_power{};
	PhaseABC<Reading<PowerFactorMillionths>> displacement_power_factor{};
	Reading<PowerFactorMillionths> total_displacement_power_factor{};
	PhaseABC<LoadNature> load_nature{};
	LoadNature total_load_nature = LoadNature::undefined;
	bool angle_reference_valid = false;
	/* Status bit 1: a merged cycle had no usable frequency reference. */
	bool phasor_invalid = false;
};
/* Decoded from the UNBALANCE-v1 record (M10): symmetrical components of
 * the fundamental phasors (a-operator conventions normative in the PL's
 * metering_types.hpp). Sequence magnitudes publish as RMS like the
 * fundamentals; angles follow the relative-to-Va convention. When the
 * block carried the phasor-invalid status bit every reading decodes with
 * MeasurementQuality::invalid. */
struct UnbalanceValues {
	Reading<MicroVolts> voltage_zero_sequence{};
	Reading<MicroVolts> voltage_positive_sequence{};
	Reading<MicroVolts> voltage_negative_sequence{};
	Reading<Millidegrees> voltage_zero_angle{};
	Reading<Millidegrees> voltage_positive_angle{};
	Reading<Millidegrees> voltage_negative_angle{};
	Reading<MicroAmperes> current_zero_sequence{};
	Reading<MicroAmperes> current_positive_sequence{};
	Reading<MicroAmperes> current_negative_sequence{};
	Reading<Millidegrees> current_zero_angle{};
	Reading<Millidegrees> current_positive_angle{};
	Reading<Millidegrees> current_negative_angle{};
	/* |X0|/|X1| and UNBL = |X2|/|X1|, gated on |X1| != 0. */
	Reading<RatioMillionths> voltage_zero_ratio{};
	Reading<RatioMillionths> voltage_unbalance{};
	Reading<RatioMillionths> current_zero_ratio{};
	Reading<RatioMillionths> current_unbalance{};
	bool angle_reference_valid = false;
	bool phasor_invalid = false;
};
struct EnergyValues {};
struct DemandValues {};
/* Power-quality event kinds and types, mirroring the PL's
 * metering_types.hpp (MET_PQ_KIND_* / MET_PQ_EVENT_*). */
enum class PowerQualityRecordKind : std::uint8_t {
	periodic = 0,   /* heartbeat snapshot, no event */
	event_start = 1,
	event_end = 2,
};
enum class PowerQualityEventType : std::uint8_t {
	none = 0,
	sag = 1,
	swell = 2,
	interruption = 3,
};

/* Decoded from the PQEVT-v1 record (M12): the sliding Urms(1/2) tier.
 * Detection conventions (thresholds as fractions of a declared reference,
 * the polyphase begin/end rule, severity, residual/peak selection) are
 * normative in the PL's metering_types.hpp. Measurement and detection are
 * independent: Urms(1/2) is measured on every enabled lane whether or not
 * `armed` is set — arming (a nonzero declared reference) only gates event
 * declaration, so a disarmed meter still reports live half-cycle RMS. */
struct PowerQualityValues {
	PowerQualityRecordKind kind = PowerQualityRecordKind::periodic;
	PowerQualityEventType event_type = PowerQualityEventType::none;
	/* Phases affected by the event, bit 0 = A, 1 = B, 2 = C. */
	std::uint8_t affected_phases = 0;
	bool armed = false;      /* a reference voltage is configured */
	bool cycle_locked = false;
	bool synthetic_half_cycle = false;
	/* Latest Urms(1/2) and the span's extremes; the span is the
	 * heartbeat window for a periodic record and the whole event for an
	 * event-end record. */
	PhaseABC<Reading<MicroVolts>> voltage{};
	PhaseABC<Reading<MicroVolts>> voltage_minimum{};
	PhaseABC<Reading<MicroVolts>> voltage_maximum{};
	PhaseABC<Reading<MicroAmperes>> current{};
	/* Ties an event START record to its END; 0 on a heartbeat. */
	std::uint32_t event_sequence = 0;
	/* Event duration in CONVERSION SAMPLES — exact, not a wall-clock
	 * estimate; divide by the record's sample rate for seconds. */
	std::uint64_t duration_samples = 0;
	std::uint32_t half_cycle_updates = 0;
	/* Threshold configuration echo, so a stored event stays readable. */
	std::uint32_t reference_micro_volts = 0;
	std::uint32_t sag_threshold_e4 = 0;
	std::uint32_t swell_threshold_e4 = 0;
	std::uint32_t interruption_threshold_e4 = 0;
	std::uint32_t hysteresis_e4 = 0;
};

struct MeterValues {
	FundamentalValues fundamental{};
	PowerValues power{};
	PhasorValues phasor{};
	UnbalanceValues unbalance{};
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
	std::optional<PhasorValues> phasor;
	std::optional<UnbalanceValues> unbalance;
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

/*
 * Decoded view of one PQEVT-v1 power-quality record (metrology M12).
 * Deliberately OUTSIDE the MeterUpdate/period store, like
 * SingleCycleSnapshot and for a sharper reason: the sliding tier has its
 * OWN sequence space, so feeding it through the period store would fight
 * the basic tier's monotone-sequence guard — and a latest-only store
 * would silently drop the START record of any event that ends before the
 * next read. Records are self-describing: `values` carries the decoded
 * measurement, the fields here its provenance.
 */
struct PowerQualitySnapshot {
	std::uint32_t sequence = 0;
	std::uint32_t configuration_generation = 0;
	std::uint32_t sample_rate_hz = 0;
	/* Span covered by this record: the heartbeat window for a periodic
	 * record, the event so far for a START, the whole event for an END. */
	std::uint64_t first_sample = 0;
	std::uint64_t last_sample = 0;
	std::uint32_t sample_count = 0;
	std::uint8_t valid_mask = 0;
	std::uint32_t status = 0;
	/* Status bit 2: first record after a discontinuity (reset, APPLY,
	 * malformed input, dropped beat) — its span does not chain onto the
	 * previous record's. */
	[[nodiscard]] bool first_after_gap() const { return (status & 0x4u) != 0u; }
	[[nodiscard]] bool arithmetic_error() const { return (status & 0x1u) != 0u; }
	PowerQualityValues values{};
};

[[nodiscard]] PowerQualitySnapshot decode_pq_event_record(
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
 * Decode a POWER-v1 (0x00070001) record into PowerValues on the Basic
 * period (it describes the same block as its BASIC-v4 sibling).
 */
MeterUpdate decode_power_meter_record(const MeterRecord &record,
				      SystemTime received_at);

/**
 * Decode a PHASOR-v1 (0x00080001) record into PhasorValues on the Basic
 * period (the third record of the same block as its BASIC-v4 sibling).
 */
MeterUpdate decode_phasor_meter_record(const MeterRecord &record,
				       SystemTime received_at);

/**
 * Decode an UNBALANCE-v1 (0x00090001) record into UnbalanceValues on the
 * Basic period (the fourth record of the same block).
 */
MeterUpdate decode_unbalance_meter_record(const MeterRecord &record,
					  SystemTime received_at);

/**
 * The aggregate tier's sibling records (M11): identical payload maps to
 * the basic-period POWER/PHASOR/UNBAL decoders, published on the
 * Cycles150_180 period with the AGG record's sequence.
 */
MeterUpdate decode_aggregate_power_meter_record(const MeterRecord &record,
						SystemTime received_at);
MeterUpdate decode_aggregate_phasor_meter_record(const MeterRecord &record,
						 SystemTime received_at);
MeterUpdate decode_aggregate_unbalance_meter_record(const MeterRecord &record,
						    SystemTime received_at);

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
 * Decode an AGG-v3 aggregate (0x00020003) record: 150/180-cycle fundamental
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
