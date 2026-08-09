#pragma once

/**
 * @file record_ingestor.hpp
 * @brief Meter-record ingest: DMA drain, validation, durability, statistics.
 */

#include "msap1/acquisition/dma/meter_record_source.hpp"
#include "msap1/meter/measurement_timebase.hpp"
#include "msap1/meter/meter_config.hpp"
#include "msap1/meter/meter_data.hpp"
#include "msap1/meter/meter_record.hpp"
#include "msap1/meter/meter_record_stream.hpp"
#include "support/time.hpp"

#include <cstdint>
#include <optional>

namespace msap1::acquisition::daemon {

/**
 * @brief Consumes meter records from the meter DMA and publishes them.
 *
 * The DMA stream interleaves basic MTR1 records with 150/180-cycle
 * aggregate MTR2 records, each on an INDEPENDENT sequence counter.
 * Responsibilities, in the order a record flows through:
 *
 *  1. Drain complete 256-byte records from the DMA reader.
 *  2. Validate each record against the ACTIVE configuration (generation,
 *     sample rate, and — for v1 records — the configured RMS window) and
 *     track continuity PER FORMAT: basic records keep sequence plus v2
 *     sample-range continuity; aggregate records get sequence continuity
 *     only (the PL already enforces sample-range continuity of the 15
 *     blocks inside an aggregate, and consecutive aggregates may
 *     legitimately be separated by aggregation resets).
 *  3. Commit the raw record to the SQLite WAL stream — durability is the
 *     publication boundary; a record is never visible to consumers before
 *     it is committed.
 *  4. Decode into the typed latest store and stamp the decoded timing's
 *     TimeQuality/UTC from the measurement timebase (identically for
 *     BlockTiming and AggregateTiming). Only BASIC records are cached as
 *     latest_record(), the instantaneous-readings source; AGGREGATE records
 *     are cached separately as latest_aggregate_record(). Time state never
 *     touches MeasurementQuality: a bad clock must not invalidate a good
 *     electrical measurement.
 *
 * The ingest counters (records, bytes, gaps, invalid, read errors) feed the
 * InfoResponse health fields; aggregate sequence gaps are tracked in their
 * own counter so the existing basic-gap health semantics stay unchanged.
 */
class MeterRecordIngestor final {
public:
	/**
	 * @param meter         Record source (the DMA reader in production).
	 * @param stream        Durable record stream (SQLite WAL).
	 * @param configuration The coordinator's ACTIVE configuration; read
	 *                      at validation time, so configuration swaps are
	 *                      picked up without re-wiring.
	 * @param timebase      UTC mapping authority for decoded timing.
	 */
	MeterRecordIngestor(msap1::acquisition::MeterRecordSource &meter,
			    msap1::MeterRecordStream &stream,
			    const msap1::PreparedMeterConfiguration &configuration,
			    const msap1::meter::MeasurementTimebase &timebase);

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
	/** @brief Latest decoded typed view for one measurement period. */
	[[nodiscard]] std::optional<msap1::MeterPeriodView>
	latest_decoded(msap1::MeasurementPeriod period) const
	{
		return meter_data_.latest(period);
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
	[[nodiscard]] std::uint64_t invalid_records() const { return invalid_records_; }
	/** @brief Missing BASIC records detected by sequence tracking. */
	[[nodiscard]] std::uint64_t sequence_gaps() const { return sequence_gaps_; }
	/** @brief Missing AGGREGATE records detected by sequence tracking. */
	[[nodiscard]] std::uint64_t aggregate_sequence_gaps() const
	{
		return aggregate_sequence_gaps_;
	}

private:
	void accept(const msap1::MeterRecord &record);
	[[nodiscard]] bool matches_configuration(
		const msap1::MeterRecord &record) const;
	[[nodiscard]] bool track_basic_continuity(
		const msap1::MeterRecord &record);
	[[nodiscard]] bool track_aggregate_continuity(
		const msap1::MeterRecord &record);

	msap1::acquisition::MeterRecordSource &meter_;
	msap1::MeterRecordStream &stream_;
	const msap1::PreparedMeterConfiguration &configuration_;
	const msap1::meter::MeasurementTimebase &timebase_;
	msap1::MeterDecoderRegistry decoders_ =
		msap1::MeterDecoderRegistry::with_builtin_decoders();
	msap1::MeterData meter_data_;
	std::uint64_t meter_records_ = 0;
	std::uint64_t dma_bytes_ = 0;
	std::uint64_t dma_read_errors_ = 0;
	std::uint64_t invalid_records_ = 0;
	std::uint64_t sequence_gaps_ = 0;
	std::uint64_t aggregate_sequence_gaps_ = 0;
	/* Continuity baselines are per format: the newest accepted basic
	 * record (also the readings cache) and the newest accepted aggregate
	 * sequence. An aggregate between two basic blocks must never look
	 * like a basic gap, and vice versa. */
	std::optional<msap1::MeterRecord> latest_record_;
	std::optional<std::uint32_t> last_aggregate_sequence_;
	std::optional<Clock::time_point> last_record_time_;
	/* Newest accepted aggregate and its arrival time. Held beside — never
	 * inside — the basic caches above so /meter/readings semantics are
	 * unchanged by aggregate publication. */
	std::optional<msap1::MeterRecord> latest_aggregate_record_;
	std::optional<Clock::time_point> last_aggregate_record_time_;
};

} // namespace msap1::acquisition::daemon
