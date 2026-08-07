#include "pipeline/record_ingestor.hpp"

#include "support/logs.hpp"

#include <chrono>
#include <exception>
#include <string>

namespace msap1::acquisition::daemon {

MeterRecordIngestor::MeterRecordIngestor(
	msap1::acquisition::MeterDmaReader &meter,
	msap1::MeterRecordStream &stream,
	const msap1::PreparedMeterConfiguration &configuration)
	: meter_(meter), stream_(stream), configuration_(configuration)
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

void MeterRecordIngestor::accept(const msap1::MeterRecord &record)
{
	if (!record.header_valid() ||
	    record.configuration_generation() !=
		configuration_.wire.generation ||
	    record.sample_rate_hz() != configuration_.wire.sample_rate_hz ||
	    record.window_samples() !=
		configuration_.wire.rms_window_samples) {
		++invalid_records_;
		return;
	}

	if (latest_record_) {
		const auto expected = latest_record_->sequence() + 1u;
		const auto received = record.sequence();
		const auto forward_distance = received - expected;
		if (forward_distance != 0u &&
		    forward_distance < (std::uint32_t{1} << 31u))
			sequence_gaps_ += forward_distance;
		else if (forward_distance != 0u) {
			/*
			 * A stale/out-of-order record is invalid, not billions
			 * of missing records. The half-range comparison keeps
			 * normal uint32 sequence wraparound valid.
			 */
			++invalid_records_;
			return;
		}
	}
	/* Durability is the publication boundary. A record is never made
	 * visible to web/CLI/publisher consumers until SQLite has committed
	 * the exact 256-byte PL record to the ordered WAL stream. */
	const auto received_at = std::chrono::system_clock::now();
	const auto cursor = stream_.append(record, received_at);
	const auto update = decoders_.decode(record, received_at);
	meter_data_.apply(update);
	latest_record_ = record;
	last_record_time_ = Clock::now();
	++meter_records_;
	(void)cursor;
}

} // namespace msap1::acquisition::daemon
