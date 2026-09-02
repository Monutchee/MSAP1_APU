#pragma once

#include "msap1/meter/meter_record.hpp"
#include "msap1/meter/meter_timing.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
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
	harmonic = 8,
	power_quality_event = 9,
	flicker = 10,
	mains_signal = 11,
	frequency_10s = 12,
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
struct MicroVarHours {};
struct MicroVoltAmpereHours {};

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

template<typename T>
struct PhaseABCTotal {
	T phase_a{};
	T phase_b{};
	T phase_c{};
	T total{};
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
/**
 * Four-quadrant fundamental reactive-energy classification.
 *
 * Deliberately avoid Q1/Q2 names: Q1 already denotes fundamental reactive
 * power in the metrology contract. Axis behavior is normative: P == 0 uses
 * the import side, while Q1 == 0 contributes to no quadrant.
 */
enum class EnergyQuadrant : std::uint8_t {
	quadrant_i = 0,
	quadrant_ii = 1,
	quadrant_iii = 2,
	quadrant_iv = 3,
	none = 0xff,
};

[[nodiscard]] constexpr EnergyQuadrant classify_energy_quadrant(
	std::int64_t active_power, std::int64_t fundamental_reactive_power) noexcept
{
	if (fundamental_reactive_power == 0)
		return EnergyQuadrant::none;
	if (fundamental_reactive_power > 0)
		return active_power < 0 ? EnergyQuadrant::quadrant_ii
					: EnergyQuadrant::quadrant_i;
	return active_power < 0 ? EnergyQuadrant::quadrant_iii
				: EnergyQuadrant::quadrant_iv;
}

struct EnergyValues {
	PhaseABCTotal<Reading<MicroWattHours>> active_import{};
	PhaseABCTotal<Reading<MicroWattHours>> active_export{};
	PhaseABCTotal<Reading<MicroVoltAmpereHours>> apparent{};
	std::array<PhaseABCTotal<Reading<MicroVarHours>>, 4>
		reactive_quadrants{};
	std::uint64_t session_id = 0;
	std::uint64_t last_sample_index = 0;
	std::uint64_t accepted_samples = 0;
	std::uint64_t skipped_samples = 0;
	std::uint32_t accepted_blocks = 0;
	std::uint32_t skipped_blocks = 0;
	std::uint64_t reset_epoch = 0;
	bool saturated = false;
	bool incomplete_input = false;
	bool discontinuity = false;

	[[nodiscard]] const PhaseABCTotal<Reading<MicroVarHours>> &reactive(
		EnergyQuadrant quadrant) const
	{
		const auto index = static_cast<std::size_t>(quadrant);
		if (index >= reactive_quadrants.size())
			throw std::out_of_range("energy quadrant");
		return reactive_quadrants[index];
	}
};

enum class DemandMethod : std::uint8_t {
	fixed_block = 0,
	sliding = 1,
};

struct DemandValues {
	PhaseABCTotal<Reading<MicroWatts>> current_active{};
	PhaseABCTotal<Reading<MicroWatts>> import_peak{};
	PhaseABCTotal<Reading<MicroWatts>> export_peak{};
	PhaseABCTotal<std::uint64_t> import_peak_sample{};
	PhaseABCTotal<std::uint64_t> export_peak_sample{};
	std::uint64_t session_id = 0;
	std::uint64_t last_sample_index = 0;
	std::uint64_t interval_anchor_sample = 0;
	std::uint32_t source_interval_count = 0;
	std::uint32_t source_status = 0;
	std::uint32_t window_seconds = 0;
	std::uint32_t update_seconds = 0;
	std::uint32_t profile_generation = 0;
	std::uint64_t peak_reset_epoch = 0;
	DemandMethod method = DemandMethod::sliding;
	bool time_aligned = false;
	bool contaminated = false;
	bool boundary_valid = false;
	bool saturated = false;
	bool incomplete_input = false;
};
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

/**
 * Audit provenance for one R5C1 FREQUENCY-10S-v1 result.
 *
 * The scalar frequency lives in FundamentalValues so every protocol adapter
 * can use the canonical attribute path. This companion preserves the exact
 * interval, observer, profile, and rejection geometry without asking a
 * consumer to reinterpret the raw 256-byte record.
 */
struct Frequency10sMetadata {
	std::uint64_t interval_end_sample_index = 0;
	std::uint64_t utc_start_nanoseconds = 0;
	std::uint64_t utc_end_nanoseconds = 0;
	std::uint64_t utc_uncertainty_nanoseconds = 0;
	std::uint32_t measured_sample_rate_millihz = 0;
	std::uint32_t source_sequence = 0;
	std::uint32_t boundary_generation = 0;
	std::uint32_t source_status = 0;
	std::uint32_t status = 0;
	std::uint32_t reasons = 0;
	std::uint32_t observer_drop_count = 0;
	std::uint8_t guard_flags = 0;
	std::uint32_t observed_crossings = 0;
	std::uint32_t included_crossings = 0;
	std::uint32_t rejected_cycles = 0;
	std::uint64_t duration_q16_samples = 0;
	std::int64_t first_crossing_q16_samples = 0;
	std::int64_t last_crossing_q16_samples = 0;
	std::uint8_t nominal_frequency_hz = 0;
	std::uint8_t reference_channel = 0;
	std::uint8_t filter_profile = 0;
	std::uint8_t calibration_profile = 0;
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
	std::optional<Frequency10sMetadata> frequency_10s;
	/* Cycle-timing identity of the source block. Present for every
	 * 10/12-cycle basic update; absent for aggregate updates.
	 * The Basic period has no fixed duration — the actual duration is
	 * sample_count / sample_rate per block (see SampleWindow). */
	std::optional<BlockTiming> timing;
	/* Aggregation identity of the source record. Present for 150/180-cycle,
	 * ten-minute, and two-hour aggregate records; basic updates leave it
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
	std::optional<Frequency10sMetadata> frequency_10s{};
};

class MeterLatestStore {
public:
	void apply(const MeterUpdate &update);

	[[nodiscard]] std::optional<MeterPeriodView>
	latest(MeasurementPeriod period) const;

private:
	static constexpr std::size_t period_count = 8;
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
 * without modifying the existing basic fundamental decoder.
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

/** Stable 128-bit M18 event identity: one R5C1 session plus its monotone ID. */
struct PowerQualityEventId {
	std::uint64_t session = 0;
	std::uint64_t counter = 0;
	bool operator==(const PowerQualityEventId &) const = default;
};

/** Canonical externally visible identity derived from the stable R5C1 ID. */
using PowerQualityEventUuid = std::array<std::byte, 16>;

/** Deterministic RFC-4122 variant/version-5 identity for API and file links. */
[[nodiscard]] PowerQualityEventUuid stable_power_quality_event_uuid(
	const PowerQualityEventId &id);

enum class PowerQualityEventLifecycle : std::uint8_t {
	start = meter_event_lifecycle_start,
	update = meter_event_lifecycle_update,
	end = meter_event_lifecycle_end,
	abort = meter_event_lifecycle_abort,
};

enum class PowerQualityLifecycleType : std::uint8_t {
	voltage_sag = 0,
	voltage_swell = 1,
	voltage_interruption = 2,
	rapid_voltage_change = 3,
	voltage_unbalance = 4,
	current_sag = 5,
	current_swell = 6,
	current_unbalance = 7,
	transient_voltage = 8,
};

/** Exact decoded view of one final R5C1 PQ-EVENT-v1 lifecycle record. */
struct PowerQualityEventLifecycleSnapshot {
	PowerQualityEventId id{};
	PowerQualityEventLifecycle lifecycle = PowerQualityEventLifecycle::start;
	PowerQualityLifecycleType type = PowerQualityLifecycleType::voltage_sag;
	std::uint8_t phase_mask = 0;
	std::uint8_t trigger_source = 0;
	std::uint32_t sequence = 0;
	std::uint32_t configuration_generation = 0;
	std::uint32_t profile_generation = 0;
	std::uint32_t sample_rate_hz = 0;
	std::uint64_t first_sample = 0;
	std::uint64_t last_sample = 0;
	std::uint64_t trigger_sample = 0;
	std::uint64_t duration_samples = 0;
	std::uint32_t valid_mask = 0;
	std::uint32_t status = 0;
	std::uint32_t threshold_e4 = 0;
	std::uint32_t hysteresis_e4 = 0;
	std::uint32_t reference_micro_units = 0;
	std::array<std::uint32_t, 3> minimum_micro_units{};
	std::array<std::uint32_t, 3> maximum_micro_units{};
	std::array<std::uint32_t, 3> current_micro_units{};
	bool waveform_enabled = false;
	bool per_phase = false;
	bool iec_classification = false;
	std::uint32_t waveform_pretrigger_ms = 0;
	std::uint32_t waveform_posttrigger_ms = 0;
	std::uint32_t waveform_decimation = 1;
	std::uint64_t start_utc_nanoseconds = 0;
	std::uint64_t last_utc_nanoseconds = 0;
	TimeQuality time_quality = TimeQuality::Unsynchronized;
	std::uint32_t discontinuities = 0;
	std::uint32_t update_count = 0;
	std::array<std::uint32_t, 4> settings_digest{};

	[[nodiscard]] bool terminal() const noexcept
	{
		return lifecycle == PowerQualityEventLifecycle::end ||
		       lifecycle == PowerQualityEventLifecycle::abort;
	}
	[[nodiscard]] bool voltage_event() const noexcept
	{
		return static_cast<std::uint8_t>(type) <= 4u ||
		       type == PowerQualityLifecycleType::transient_voltage;
	}
};

[[nodiscard]] PowerQualityEventLifecycleSnapshot
decode_pq_event_lifecycle_record(const MeterRecord &record);

enum class FlickerRecordKind : std::uint8_t {
	live = meter_flicker_kind_live,
	pst = meter_flicker_kind_pst,
	plt = meter_flicker_kind_plt,
};

/** Exact decoded view of one final R5C1 FLICKER-v1 record. */
struct FlickerSnapshot {
	FlickerRecordKind kind = FlickerRecordKind::live;
	std::uint32_t sequence = 0;
	std::uint32_t configuration_generation = 0;
	std::uint32_t profile_generation = 0;
	std::uint32_t sample_rate_hz = 0;
	std::uint64_t first_sample = 0;
	std::uint64_t last_sample = 0;
	std::uint32_t sample_count = 0;
	std::uint32_t interval_seconds = 0;
	std::uint8_t phase_valid_mask = 0;
	std::uint16_t lamp_voltage = 0;
	std::uint8_t nominal_frequency_hz = 0;
	std::array<std::uint32_t, 3> pinst_q16{};
	std::array<std::uint32_t, 3> pst_q16{};
	std::array<std::uint32_t, 3> plt_q16{};
	std::array<std::uint32_t, 3> valid_internal_samples{};
	std::uint32_t status = 0;
	std::uint32_t source_status = 0;

	[[nodiscard]] bool first_after_gap() const noexcept
	{
		return (status & (1u << 2u)) != 0u;
	}
	[[nodiscard]] bool arithmetic_error() const noexcept
	{
		return (status & 1u) != 0u;
	}
};

[[nodiscard]] FlickerSnapshot decode_flicker_record(
	const MeterRecord &record);

/** Exact decoded view of one final R5C1 MAINS-SIGNAL-v1 record. */
struct MainsSignalSnapshot {
	std::uint32_t sequence = 0;
	std::uint32_t configuration_generation = 0;
	std::uint32_t profile_generation = 0;
	std::uint32_t sample_rate_hz = 0;
	std::uint64_t first_sample = 0;
	std::uint64_t last_sample = 0;
	std::uint32_t sample_count = 0;
	std::uint8_t phase_valid_mask = 0;
	std::uint8_t detected_phase_mask = 0;
	std::uint32_t configured_millihz = 0;
	std::uint32_t measured_millihz = 0;
	std::array<std::uint32_t, 3> magnitude_microvolts{};
	std::array<std::uint32_t, 3> background_microvolts{};
	std::uint32_t bandwidth_millihz = 0;
	std::uint32_t observation_ms = 0;
	std::uint32_t threshold_e4 = 0;
	std::uint32_t reference_microvolts = 0;
	std::uint32_t status = 0;
	std::uint32_t source_status = 0;

	[[nodiscard]] bool any_detected() const noexcept
	{
		return detected_phase_mask != 0;
	}
	[[nodiscard]] bool first_after_gap() const noexcept
	{
		return (status & (1u << 2u)) != 0u;
	}
	[[nodiscard]] bool arithmetic_error() const noexcept
	{
		return (status & 1u) != 0u;
	}
};

[[nodiscard]] MainsSignalSnapshot decode_mains_signal_record(
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

/** Ten-minute sibling payloads use the aggregate word maps and publish on
 * the independently aligned Min10 period. Contaminated or unaligned
 * intervals remain decodable for diagnostics but carry invalid quality. */
MeterUpdate decode_ten_minute_power_meter_record(const MeterRecord &record,
						 SystemTime received_at);
MeterUpdate decode_ten_minute_phasor_meter_record(const MeterRecord &record,
						  SystemTime received_at);
MeterUpdate decode_ten_minute_unbalance_meter_record(
	const MeterRecord &record, SystemTime received_at);

/** Two-hour sibling payloads retain the aggregate word maps and publish on
 * the independent Hour2 period. */
MeterUpdate decode_two_hour_power_meter_record(const MeterRecord &record,
					       SystemTime received_at);
MeterUpdate decode_two_hour_phasor_meter_record(const MeterRecord &record,
					        SystemTime received_at);
MeterUpdate decode_two_hour_unbalance_meter_record(
	const MeterRecord &record, SystemTime received_at);

/** Non-normative live-partial sibling payloads. */
MeterUpdate decode_ten_minute_open_power_meter_record(
	const MeterRecord &record, SystemTime received_at);
MeterUpdate decode_ten_minute_open_phasor_meter_record(
	const MeterRecord &record, SystemTime received_at);
MeterUpdate decode_ten_minute_open_unbalance_meter_record(
	const MeterRecord &record, SystemTime received_at);
MeterUpdate decode_two_hour_open_power_meter_record(
	const MeterRecord &record, SystemTime received_at);
MeterUpdate decode_two_hour_open_phasor_meter_record(
	const MeterRecord &record, SystemTime received_at);
MeterUpdate decode_two_hour_open_unbalance_meter_record(
	const MeterRecord &record, SystemTime received_at);

/**
 * Decode a BASIC-v4 (0x00010004) record: fundamental values plus the
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
 * Decode the R5C1-authoritative FREQUENCY-10S-v1 record. Unlike PL-origin
 * records, its UTC endpoints and uncertainty are part of the signed-off wire
 * result and must never be restamped from the APU's current timebase.
 */
[[nodiscard]] MeterUpdate decode_frequency_10s_meter_record(
	const MeterRecord &record, SystemTime received_at =
					     std::chrono::system_clock::now());

/**
 * Decode an AGG-v3 aggregate (0x00020003) record: 150/180-cycle fundamental
 * values plus the AggregateTiming identity. The PL is the authoritative
 * aggregator — this only DECODES what the PL computed; the APU never
 * recomputes aggregate values. As for the basic record, TimeQuality and utc_start are
 * not in the record: the caller (the ingestor) stamps them from the
 * MeasurementTimebase after decoding.
 */
[[nodiscard]] MeterUpdate decode_aggregate_meter_record(
	const MeterRecord &record, SystemTime received_at =
					     std::chrono::system_clock::now());

/** Decode a clock-aligned ten-minute aggregate and preserve both the target
 * and actual close boundary. The first startup interval may be emitted as
 * contaminated; callers can inspect its provenance, while every electrical
 * reading is explicitly invalid. */
[[nodiscard]] MeterUpdate decode_ten_minute_meter_record(
	const MeterRecord &record, SystemTime received_at =
					     std::chrono::system_clock::now());

/** Decode a two-hour aggregate built from twelve complete, consecutive,
 * boundary-clean ten-minute accumulator images. */
[[nodiscard]] MeterUpdate decode_two_hour_meter_record(
	const MeterRecord &record, SystemTime received_at =
					     std::chrono::system_clock::now());

/** Decode non-normative views of the currently open accumulators. */
[[nodiscard]] MeterUpdate decode_ten_minute_open_meter_record(
	const MeterRecord &record, SystemTime received_at =
					     std::chrono::system_clock::now());
[[nodiscard]] MeterUpdate decode_two_hour_open_meter_record(
	const MeterRecord &record, SystemTime received_at =
					     std::chrono::system_clock::now());

} // namespace msap1
