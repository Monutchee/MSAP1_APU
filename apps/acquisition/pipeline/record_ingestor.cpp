#include "pipeline/record_ingestor.hpp"

#include "support/logs.hpp"

#include <chrono>
#include <exception>
#include <string>

namespace msap1::acquisition::daemon {

MeterRecordIngestor::MeterRecordIngestor(
	msap1::acquisition::MeterRecordSource &meter,
	msap1::MeterRecordStream &stream,
	const msap1::PreparedMeterConfiguration &configuration,
	const msap1::meter::MeasurementTimebase &timebase)
	: meter_(meter), stream_(stream), configuration_(configuration),
	  timebase_(timebase)
{
}

void MeterRecordIngestor::read_available()
{
	msap1::acquisition::MeterRecordBatch batch{};
	try {
		batch = meter_.read_available();
	} catch (const std::exception &error) {
		++dma_read_errors_;
		log_message(dma_log, mnc::logging::Priority::error,
			"meter DMA read failed: " + std::string(error.what()),
			"dma_read_failed");
		return;
	}
	if (batch.bytes == 0)
		return;
	dma_bytes_ += batch.bytes;
	if (batch.partial_record) {
		++invalid_records_;
		log_message(dma_log, mnc::logging::Priority::warning,
			"meter DMA returned a partial record",
			"dma_partial_record",
			{{"MNC_DMA_BYTES", std::to_string(batch.bytes)}});
		return;
	}
	for (std::size_t index = 0; index < batch.count; ++index)
		accept(batch.records[index]);
}

void MeterRecordIngestor::begin_epoch()
{
	latest_record_.reset();
	last_record_time_.reset();
	sequence_gaps_ = 0;
}

void MeterRecordIngestor::clear_latest()
{
	latest_record_.reset();
	last_record_time_.reset();
}

bool MeterRecordIngestor::matches_configuration(
	const msap1::MeterRecord &record) const
{
	if (!record.header_valid() ||
	    record.configuration_generation() !=
		configuration_.wire.generation ||
	    record.sample_rate_hz() != configuration_.wire.sample_rate_hz)
		return false;
	/*
	 * Only v1 records must echo the configured window: a v2 block is
	 * cycle-defined, so its word-6 sample count legitimately varies with
	 * grid frequency and matches the configured value only in free-run.
	 */
	if (record.record_format() == msap1::meter_periodic_format &&
	    record.window_samples() != configuration_.wire.rms_window_samples)
		return false;
	return true;
}

void MeterRecordIngestor::accept(const msap1::MeterRecord &record)
{
	if (!matches_configuration(record)) {
		++invalid_records_;
		return;
	}

	bool sequence_continuous = latest_record_.has_value();
	if (latest_record_) {
		const auto expected = latest_record_->sequence() + 1u;
		const auto received = record.sequence();
		const auto forward_distance = received - expected;
		if (forward_distance != 0u &&
		    forward_distance < (std::uint32_t{1} << 31u)) {
			sequence_gaps_ += forward_distance;
			sequence_continuous = false;
		} else if (forward_distance != 0u) {
			/*
			 * A stale/out-of-order record is invalid, not billions
			 * of missing records. The half-range comparison keeps
			 * normal uint32 sequence wraparound valid.
			 */
			++invalid_records_;
			return;
		}
	}
	/*
	 * v2 sample-range continuity: blocks are gapless by construction on
	 * the PL conversion-domain counter, so between consecutive sequences
	 * first(N+1) must equal first(N) + count(N). A mismatch means lost
	 * samples that the sequence numbers did not reveal; count it as a gap
	 * and resync — this record still becomes the new continuity baseline,
	 * exactly like sequence-gap handling.
	 */
	if (sequence_continuous &&
	    record.record_format() == msap1::meter_periodic_format_v2 &&
	    latest_record_->record_format() == msap1::meter_periodic_format_v2) {
		const auto expected_first = latest_record_->first_sample_index() +
			latest_record_->block_sample_count();
		if (record.first_sample_index() != expected_first) {
			++sequence_gaps_;
			log_message(dma_log, mnc::logging::Priority::warning,
				"meter record sample range is discontinuous: expected " +
					std::to_string(expected_first) + ", got " +
					std::to_string(record.first_sample_index()),
				"meter_sample_range_gap",
				{{"MNC_EXPECTED_SAMPLE_INDEX",
				  std::to_string(expected_first)},
				 {"MNC_FIRST_SAMPLE_INDEX",
				  std::to_string(record.first_sample_index())}});
		}
	}
	/*
	 * Decode-validate BEFORE durability: a record whose timing fields are
	 * malformed (zero-sample block, overflowing sample range, impossible
	 * cycle count) is invalid exactly like a failed configuration match —
	 * counted, logged, and never committed, published, or allowed to
	 * become the continuity baseline. Only validated records enter the
	 * WAL stream.
	 */
	const auto received_at = std::chrono::system_clock::now();
	msap1::MeterUpdate update;
	try {
		update = decoders_.decode(record, received_at);
	} catch (const std::exception &error) {
		++invalid_records_;
		log_message(dma_log, mnc::logging::Priority::warning,
			"meter record rejected by decoder: " +
				std::string(error.what()),
			"meter_record_decode_rejected",
			{{"MNC_SEQUENCE", std::to_string(record.sequence())}});
		return;
	}
	/* Durability is the publication boundary. A record is never made
	 * visible to web/CLI/publisher consumers until SQLite has committed
	 * the exact 256-byte PL record to the ordered WAL stream. */
	const auto cursor = stream_.append(record, received_at);
	if (update.timing) {
		/*
		 * Stamp UTC state at decode time — the PL cannot know it.
		 * This touches only BlockTiming: TimeQuality must never mark
		 * the electrical MeasurementQuality invalid. The block's own
		 * configuration generation keys the mapping, so a sync point
		 * from another generation can never mislabel this block.
		 */
		const auto now = Clock::now();
		update.timing->time_quality = timebase_.quality(now);
		const auto estimate = timebase_.utc_for_sample(
			update.timing->first_sample_index,
			update.timing->configuration_generation, now);
		/* Timestamp and its error bound travel together: both set or
		 * both absent. */
		if (estimate) {
			update.timing->utc_start = estimate->utc;
			update.timing->utc_uncertainty_ns =
				estimate->uncertainty_ns;
		}
	}
	meter_data_.apply(update);
	latest_record_ = record;
	last_record_time_ = Clock::now();
	++meter_records_;
	(void)cursor;
}

} // namespace msap1::acquisition::daemon
