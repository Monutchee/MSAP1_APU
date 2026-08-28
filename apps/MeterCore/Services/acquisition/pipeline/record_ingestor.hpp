#pragma once

/**
 * @file record_ingestor.hpp
 * @brief Meter-record ingest: DMA drain, validation, publication, statistics.
 */

#include "msap1/acquisition/dma/meter_record_source.hpp"
#include "pipeline/record_interval_category.hpp"
#include "msap1/meter/measurement_timebase.hpp"
#include "msap1/meter/harmonic_spectrum.hpp"
#include "msap1/meter/meter_config.hpp"
#include "msap1/meter/meter_data.hpp"
#include "msap1/meter/meter_record.hpp"
#include "mnc/MeterDataProvider/stream/meter_record_publisher.hpp"
#include "support/time.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace msap1::acquisition::daemon {

/**
 * @brief Consumes meter records from the meter DMA and publishes them.
 *
 * The DMA stream interleaves basic MTR1, 150/180-cycle aggregate MTR2,
 * clock-aligned ten-minute M13 records, and two-hour M14 records, each on an
 * INDEPENDENT sequence counter.
 * Responsibilities, in the order a record flows through:
 *
 *  1. Drain complete 256-byte records from the DMA reader.
 *  2. Validate each record against the ACTIVE configuration (generation
 *     and sample rate; blocks are cycle-defined, so there is no window
 *     echo to match) and track continuity PER FORMAT: basic records keep
 *     sequence plus sample-range continuity; aggregate records get sequence
 *     continuity only (the PL already enforces sample-range continuity of
 *     the 15 blocks inside an aggregate, and consecutive aggregates may
 *     legitimately be separated by aggregation resets).
 *  3. Decode into the typed latest store and stamp the decoded timing's
 *     TimeQuality/UTC from the measurement timebase (identically for
 *     BlockTiming and AggregateTiming). Only BASIC records are cached as
 *     latest_record(), the instantaneous-readings source; AGGREGATE records
 *     are cached separately as latest_aggregate_record(), together with the
 *     TimeQuality stamped onto that aggregate — provenance belongs to the
 *     measurement, not to whenever a consumer reads it back. Time state
 *     never touches MeasurementQuality: a bad clock must not invalidate a
 *     good electrical measurement.
 *
 * The ingest counters (records, bytes, gaps, invalid, read errors) feed the
 * InfoResponse health fields; aggregate sequence gaps are tracked in their
 * own counter so the existing basic-gap health semantics stay unchanged.
 */
class MeterRecordIngestor final {
public:
	/**
	 * @param meter         Record source (the DMA reader in production).
	 * @param configuration The coordinator's ACTIVE configuration; read
	 *                      at validation time, so configuration swaps are
	 *                      picked up without re-wiring.
	 * @param timebase      UTC mapping authority for decoded timing.
	 */
	MeterRecordIngestor(msap1::acquisition::MeterRecordSource &meter,
			    const msap1::PreparedMeterConfiguration &configuration,
			    const msap1::meter::MeasurementTimebase &timebase,
			    mnc::meter_stream::MeterRecordPublisher &publisher);

	/** @brief Drain and process every complete record the DMA has ready. */
	void read_available();

	/**
	 * @brief Start a new continuity epoch (deliberate DMA/capture restart).
	 *
	 * Coordinated configurations may reset or advance PL sequences while
	 * DMA is stopped; that boundary is not packet loss and must not
	 * increment the sequence-gap counter.
	 */
	void begin_epoch();

	/** @brief Forget cached records/baselines after a configuration swap. */
	void clear_latest();

	/** @brief Count one DMA transport failure (POLLERR/disconnect path). */
	void note_dma_failure() { ++dma_read_errors_; }

	/**
	 * @brief Newest accepted BASIC record (raw wire form).
	 *
	 * Deliberately never an aggregate: this cache feeds the CLI/REST
	 * instantaneous readings, which must not flip between ~200 ms basic
	 * and ~3 s aggregate quantities as records interleave.
	 */
	[[nodiscard]] const std::optional<msap1::MeterRecord> &
	latest_record() const
	{
		return latest_record_;
	}
	/**
	 * @brief Newest accepted AGGREGATE (MTR2) record (raw wire form).
	 *
	 * The 150/180-cycle counterpart of latest_record(): a separate cache
	 * on its own record stream, so exposing aggregates never changes what
	 * the instantaneous-readings path sees.
	 */
	[[nodiscard]] const std::optional<msap1::MeterRecord> &
	latest_aggregate_record() const
	{
		return latest_aggregate_record_;
	}
	/**
	 * @brief UTC synchronization state stamped onto the cached aggregate.
	 *
	 * The quality that applied WHEN latest_aggregate_record() was
	 * ingested, not the timebase's current one: an aggregate measured
	 * while synchronized stays labelled synchronized even if the clock
	 * later drops into holdover. Meaningful only while
	 * latest_aggregate_record() holds a value; it is reset with that
	 * cache at every deliberate boundary.
	 */
	[[nodiscard]] msap1::meter::TimeQuality
	latest_aggregate_time_quality() const
	{
		return latest_aggregate_time_quality_;
	}
	/** @brief Latest decoded typed view for one measurement period. */
	[[nodiscard]] std::optional<msap1::MeterPeriodView>
	latest_decoded(msap1::MeasurementPeriod period) const
	{
		return meter_data_.latest(period);
	}
	/** Typed latest-state source used by the product snapshot provider. */
	[[nodiscard]] msap1::MeterData &meter_data() noexcept
	{
		return meter_data_;
	}
	/** @brief Milliseconds since the last accepted record. */
	[[nodiscard]] std::uint32_t record_age_ms() const
	{
		return age_milliseconds(last_record_time_);
	}
	/**
	 * @brief Milliseconds since the last accepted AGGREGATE record.
	 *
	 * Tracked separately because the two streams have very different
	 * cadences: reporting the ~200 ms basic freshness for a ~3 s aggregate
	 * would make a stale aggregate look fresh.
	 */
	[[nodiscard]] std::uint32_t aggregate_record_age_ms() const
	{
		return age_milliseconds(last_aggregate_record_time_);
	}
	[[nodiscard]] std::uint64_t meter_records() const { return meter_records_; }
	[[nodiscard]] std::uint64_t dma_bytes() const { return dma_bytes_; }
	[[nodiscard]] std::uint64_t dma_read_errors() const { return dma_read_errors_; }
	/** Rejections since the most recent deliberate capture epoch began. */
	[[nodiscard]] std::uint64_t invalid_records() const { return invalid_records_; }
	/** Process-lifetime rejection total retained for forensic diagnostics. */
	[[nodiscard]] std::uint64_t lifetime_invalid_records() const
	{
		return lifetime_invalid_records_;
	}
	/** @brief Missing BASIC records detected by sequence tracking. */
	[[nodiscard]] std::uint64_t sequence_gaps() const { return sequence_gaps_; }
	[[nodiscard]] std::uint64_t single_cycle_records() const
	{
		return single_cycle_records_;
	}
	[[nodiscard]] const std::optional<msap1::SingleCycleSnapshot> &
	latest_single_cycle() const
	{
		return latest_single_cycle_;
	}
	/** @brief Accepted PQEVT records (heartbeats and event edges). */
	[[nodiscard]] std::uint64_t pq_event_records() const
	{
		return pq_event_records_;
	}
	/** @brief Events DECLARED, counted on the START edge. */
	[[nodiscard]] std::uint64_t pq_events() const { return pq_events_; }
	/** @brief Latest PQEVT record of any kind: live Urms(1/2). */
	[[nodiscard]] const std::optional<msap1::PowerQualitySnapshot> &
	latest_power_quality() const
	{
		return latest_power_quality_;
	}
	/**
	 * @brief Latest PQEVT record that was an event edge.
	 *
	 * Held apart from latest_power_quality() so the heartbeat stream
	 * cannot erase a short event before anything reads it.
	 */
	[[nodiscard]] const std::optional<msap1::PowerQualitySnapshot> &
	latest_power_quality_event() const
	{
		return latest_power_quality_event_;
	}
	/** @brief Accepted HARMONIC-v1 chunks, including incomplete families. */
	[[nodiscard]] std::uint64_t harmonic_records() const
	{
		return harmonic_records_;
	}
	/** @brief Complete 42-record spectrum families made externally visible. */
	[[nodiscard]] std::uint64_t harmonic_families() const
	{
		return harmonic_families_;
	}
	/** @brief Partial or wholly skipped producer families. */
	[[nodiscard]] std::uint64_t incomplete_harmonic_families() const
	{
		return incomplete_harmonic_families_;
	}
	[[nodiscard]] const std::optional<msap1::HarmonicSpectrumSnapshot> &
	latest_harmonic_spectrum() const
	{
		return latest_harmonic_spectra_[0];
	}
	[[nodiscard]] const std::optional<msap1::HarmonicSpectrumSnapshot> &
	latest_harmonic_spectrum(msap1::MeasurementPeriod period) const;
	[[nodiscard]] std::uint64_t harmonic_records(
		msap1::MeasurementPeriod period) const;
	[[nodiscard]] std::uint64_t harmonic_families(
		msap1::MeasurementPeriod period) const;
	[[nodiscard]] std::uint64_t incomplete_harmonic_families(
		msap1::MeasurementPeriod period) const;
	/** @brief Missing AGGREGATE records detected by sequence tracking. */
	[[nodiscard]] std::uint64_t aggregate_sequence_gaps() const
	{
		return aggregate_sequence_gaps_;
	}
	/** @brief Missing TEN-MINUTE records detected on the M13 stream. */
	[[nodiscard]] std::uint64_t ten_minute_sequence_gaps() const
	{
		return ten_minute_sequence_gaps_;
	}
	/** @brief Missing TWO-HOUR records detected on the M14 stream. */
	[[nodiscard]] std::uint64_t two_hour_sequence_gaps() const
	{
		return two_hour_sequence_gaps_;
	}
	/**
	 * @brief Kernel transport accounting, read live from the source.
	 *
	 * Not an ingest counter: these totals belong to the driver and are
	 * surfaced beside the ingest ones because only their combination says
	 * where a record was lost. Sampled on demand, so a caller building a
	 * reply gets the ring's state at reply time.
	 */
	[[nodiscard]] msap1::acquisition::MeterTransportStatus
	transport_status() const
	{
		return meter_.transport_status();
	}

private:
	void accept(const msap1::MeterRecord &record);
	void note_invalid_record()
	{
		++invalid_records_;
		++lifetime_invalid_records_;
	}
	/** Rate-limited raw-word forensics shared by every silent rejection
	 * path — the datum that localizes a PL emission fault. */
	void log_rejected_record(const std::string &reason, const char *event,
		const msap1::MeterRecord &record);
	/** Derive which matches_configuration() check failed, then dump. */
	void log_configuration_mismatch(const msap1::MeterRecord &record);
	[[nodiscard]] bool matches_configuration(
		const msap1::MeterRecord &record) const;
	[[nodiscard]] bool track_basic_continuity(
		const msap1::MeterRecord &record);
	[[nodiscard]] bool track_aggregate_continuity(
		const msap1::MeterRecord &record);
	[[nodiscard]] bool track_ten_minute_continuity(
		const msap1::MeterRecord &record);
	[[nodiscard]] bool track_two_hour_continuity(
		const msap1::MeterRecord &record);

	msap1::acquisition::MeterRecordSource &meter_;
	const msap1::PreparedMeterConfiguration &configuration_;
	const msap1::meter::MeasurementTimebase &timebase_;
	mnc::meter_stream::MeterRecordPublisher &publisher_;
	msap1::MeterDecoderRegistry decoders_ =
		msap1::MeterDecoderRegistry::with_builtin_decoders();
	msap1::MeterData meter_data_;
	std::uint64_t meter_records_ = 0;
	std::uint64_t dma_bytes_ = 0;
	std::uint64_t dma_read_errors_ = 0;
	/* Health uses the current capture epoch; the lifetime total remains
	 * observable without making a recovered pipeline permanently unhealthy. */
	std::uint64_t invalid_records_ = 0;
	std::uint64_t lifetime_invalid_records_ = 0;
	std::uint64_t sequence_gaps_ = 0;
	/* Single-cycle diagnostic records: acceptance count and the latest
	 * decoded snapshot (verification tooling; never published to the
	 * WAL/period stores). */
	std::uint64_t single_cycle_records_ = 0;
	std::optional<msap1::SingleCycleSnapshot> latest_single_cycle_{};
	/* Power-quality records (metrology M12): the sliding tier runs its
	 * OWN sequence space on the shared DMA stream, so it is counted and
	 * cached here and never continuity-tracked against basic/aggregate. */
	std::uint64_t pq_event_records_ = 0;
	std::uint64_t pq_events_ = 0;
	std::optional<msap1::PowerQualitySnapshot> latest_power_quality_{};
	std::optional<msap1::PowerQualitySnapshot> latest_power_quality_event_{};
	/* M16 publishes one spectrum only after all 42 channel/chunk records
	 * agree. The bounded assembler and publication buffer each retain at most
	 * one partial family; incomplete/mismatched families never reach the
	 * durable stream. */
	struct PendingHarmonicFamily {
		std::uint32_t sequence = 0;
		std::array<std::optional<mnc::meter_stream::MeterStreamRecord>,
			msap1::harmonic_records_per_family> records{};
		std::size_t count = 0;
	};
	static constexpr std::size_t harmonic_period_count = 4;
	std::array<msap1::HarmonicFamilyAssembler, harmonic_period_count>
		harmonic_assemblers_{};
	std::array<std::optional<PendingHarmonicFamily>, harmonic_period_count>
		pending_harmonic_families_{};
	std::uint64_t harmonic_records_ = 0;
	std::uint64_t harmonic_families_ = 0;
	std::uint64_t incomplete_harmonic_families_ = 0;
	std::array<std::uint64_t, harmonic_period_count>
		harmonic_records_by_period_{};
	std::array<std::uint64_t, harmonic_period_count>
		harmonic_families_by_period_{};
	std::array<std::uint64_t, harmonic_period_count>
		incomplete_harmonic_families_by_period_{};
	std::array<std::optional<msap1::HarmonicSpectrumSnapshot>,
		harmonic_period_count> latest_harmonic_spectra_{};
	std::uint64_t aggregate_sequence_gaps_ = 0;
	std::uint64_t ten_minute_sequence_gaps_ = 0;
	std::uint64_t two_hour_sequence_gaps_ = 0;
	/* Kernel transport-overrun total at the last sequence-gap log. The
	 * delta at gap time is what attributes the loss: it either matches the
	 * gap (kernel ring overrun) or stays zero (upstream/PL loss). */
	std::uint64_t last_transport_overruns_ = 0;
	/* Rate-limit independently per interval category. A storm stays bounded,
	 * while a 10/12-cycle rejection can never hide or absorb the count for a
	 * 150/180-cycle, ten-minute, or two-hour rejection. */
	struct RejectLogState {
		std::optional<Clock::time_point> last_log{};
		std::uint64_t suppressed = 0;
	};
	std::array<RejectLogState, record_interval_category_count>
		reject_log_states_{};
	/* Continuity baselines are per format: the newest accepted basic
	 * record (also the readings cache), the newest accepted aggregate
	 * sequence, the newest accepted ten-minute sequence, and the newest
	 * accepted two-hour sequence. A record from one stream must never look
	 * like a gap or stale record in another. */
	std::optional<msap1::MeterRecord> latest_record_;
	std::optional<std::uint32_t> last_aggregate_sequence_;
	std::optional<std::uint32_t> last_ten_minute_sequence_;
	std::optional<std::uint32_t> last_two_hour_sequence_;
	std::optional<Clock::time_point> last_record_time_;
	/* Newest accepted aggregate and its arrival time. Held beside — never
	 * inside — the basic caches above so /meter/readings semantics are
	 * unchanged by aggregate publication. */
	std::optional<msap1::MeterRecord> latest_aggregate_record_;
	std::optional<Clock::time_point> last_aggregate_record_time_;
	/* Measurement-time provenance of latest_aggregate_record_: the
	 * quality stamped onto its decoded AggregateTiming, captured at
	 * decode. Kept beside the record so a consumer polling minutes later
	 * cannot relabel a finished measurement with the clock's current
	 * state. Reset with the cache, so a value is only ever read while an
	 * aggregate is present. */
	msap1::meter::TimeQuality latest_aggregate_time_quality_ =
		msap1::meter::TimeQuality::Unsynchronized;
};

} // namespace msap1::acquisition::daemon
