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
 * @brief Consumes MTR1 records from the meter DMA and publishes them.
 *
 * Responsibilities, in the order a record flows through:
 *
 *  1. Drain complete 256-byte records from the DMA reader.
 *  2. Validate each record against the ACTIVE configuration (generation,
 *     sample rate, and — for v1 records — the configured RMS window) and
 *     track sequence plus v2 sample-range continuity.
 *  3. Commit the raw record to the SQLite WAL stream — durability is the
 *     publication boundary; a record is never visible to consumers before
 *     it is committed.
 *  4. Decode into the typed latest store, stamp the decoded block's
 *     TimeQuality/UTC from the measurement timebase, and cache it as
 *     latest_record(). Time state never touches MeasurementQuality: a bad
 *     clock must not invalidate a good electrical measurement.
 *
 * The ingest counters (records, bytes, gaps, invalid, read errors) feed the
 * InfoResponse health fields.
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

	/** @brief Forget the cached record after a configuration swap. */
	void clear_latest();

	/** @brief Count one DMA transport failure (POLLERR/disconnect path). */
	void note_dma_failure() { ++dma_read_errors_; }

	[[nodiscard]] const std::optional<msap1::MeterRecord> &
	latest_record() const
	{
		return latest_record_;
	}
	/** @brief Milliseconds since the last accepted record. */
	[[nodiscard]] std::uint32_t record_age_ms() const
	{
		return age_milliseconds(last_record_time_);
	}
	[[nodiscard]] std::uint64_t meter_records() const { return meter_records_; }
	[[nodiscard]] std::uint64_t dma_bytes() const { return dma_bytes_; }
	[[nodiscard]] std::uint64_t dma_read_errors() const { return dma_read_errors_; }
	[[nodiscard]] std::uint64_t invalid_records() const { return invalid_records_; }
	[[nodiscard]] std::uint64_t sequence_gaps() const { return sequence_gaps_; }

private:
	void accept(const msap1::MeterRecord &record);
	[[nodiscard]] bool matches_configuration(
		const msap1::MeterRecord &record) const;

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
	std::optional<msap1::MeterRecord> latest_record_;
	std::optional<Clock::time_point> last_record_time_;
};

} // namespace msap1::acquisition::daemon
