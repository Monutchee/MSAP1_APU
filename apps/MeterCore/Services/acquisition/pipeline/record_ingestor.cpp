#include "pipeline/record_ingestor.hpp"

#include "support/logs.hpp"

#include <chrono>
#include <exception>
#include <string>

namespace msap1::acquisition::daemon {
namespace {

/*
 * Stamp UTC state at decode time — the PL cannot know it. This touches only
 * the timing identity: TimeQuality must never mark the electrical
 * MeasurementQuality invalid. The record's own configuration generation
 * keys the mapping, so a sync point from another generation can never
 * mislabel the record. BlockTiming and AggregateTiming share these fields
 * by design, so basic and aggregate records are stamped identically.
 */
template<typename Timing>
void stamp_time_state(Timing &timing,
		      const msap1::meter::MeasurementTimebase &timebase)
{
	const auto now = Clock::now();
	timing.time_quality = timebase.quality(now);
	const auto estimate = timebase.utc_for_sample(
		timing.first_sample_index, timing.configuration_generation, now);
	/* Timestamp and its error bound travel together: both set or both
	 * absent. */
	if (estimate) {
		timing.utc_start = estimate->utc;
		timing.utc_uncertainty_ns = estimate->uncertainty_ns;
	}
}

} // namespace

MeterRecordIngestor::MeterRecordIngestor(
	msap1::acquisition::MeterRecordSource &meter,
	const msap1::PreparedMeterConfiguration &configuration,
	const msap1::meter::MeasurementTimebase &timebase,
	mnc::meter_stream::MeterRecordPublisher &publisher)
	: meter_(meter), configuration_(configuration),
	  timebase_(timebase), publisher_(publisher)
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
	last_aggregate_sequence_.reset();
	latest_aggregate_record_.reset();
	latest_aggregate_time_quality_ =
		msap1::meter::TimeQuality::Unsynchronized;
	last_record_time_.reset();
	last_aggregate_record_time_.reset();
	sequence_gaps_ = 0;
	aggregate_sequence_gaps_ = 0;
}

void MeterRecordIngestor::clear_latest()
{
	/* A configuration swap is a deliberate boundary for BOTH record
	 * streams: the PL may reset either sequence while reconfiguring, and
	 * that must not be counted as packet loss. */
	latest_record_.reset();
	last_aggregate_sequence_.reset();
	latest_aggregate_record_.reset();
	latest_aggregate_time_quality_ =
		msap1::meter::TimeQuality::Unsynchronized;
	last_record_time_.reset();
	last_aggregate_record_time_.reset();
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

/*
 * Basic (MTR1) continuity: wire-sequence tracking against the newest
 * accepted basic record, plus — for consecutive v2 sequences — sample-range
 * continuity on the PL conversion-domain counter. Interleaved aggregate
 * records never participate: they neither advance nor break this baseline.
 */
bool MeterRecordIngestor::track_basic_continuity(
	const msap1::MeterRecord &record)
{
	bool sequence_continuous = latest_record_.has_value();
	if (latest_record_) {
		const auto expected = latest_record_->sequence() + 1u;
		const auto received = record.sequence();
		const auto forward_distance = received - expected;
		if (forward_distance != 0u &&
		    forward_distance < (std::uint32_t{1} << 31u)) {
			sequence_gaps_ += forward_distance;
			sequence_continuous = false;
			/*
			 * Lost basic records were previously counted and never
			 * logged, so the only symptom was a sequence_gaps total
			 * with no timestamp and no cause. Log it like every
			 * other continuity fault. A gap here also appears
			 * whenever the PREVIOUS record was rejected by the
			 * decoder — that record never became the baseline — so
			 * this is what makes a rejection traceable instead of
			 * silently shifting the count.
			 *
			 * The kernel transport-overrun delta is captured with the
			 * gap because it decides where the record was lost: the
			 * kernel ring drops records only when userspace falls
			 * behind, so a gap with no overrun growth happened
			 * upstream of the ring (PL or rejection), and a gap with
			 * matching growth is a consumer stall — two entirely
			 * different investigations.
			 */
			const auto overruns = meter_.transport_overruns();
			const auto overrun_delta =
				overruns - last_transport_overruns_;
			last_transport_overruns_ = overruns;
			log_message(dma_log, mnc::logging::Priority::warning,
				"meter record sequence gap: expected " +
					std::to_string(expected) + ", got " +
					std::to_string(received) +
					(overrun_delta != 0
						 ? " (kernel transport overran " +
						   std::to_string(overrun_delta) +
						   " records)"
						 : " (no kernel transport overrun)"),
				"meter_sequence_gap",
				{{"MNC_EXPECTED_SEQUENCE", std::to_string(expected)},
				 {"MNC_SEQUENCE", std::to_string(received)},
				 {"MNC_MISSING_RECORDS",
				  std::to_string(forward_distance)},
				 {"MNC_TRANSPORT_OVERRUNS",
				  std::to_string(overruns)},
				 {"MNC_TRANSPORT_OVERRUN_DELTA",
				  std::to_string(overrun_delta)}});
		} else if (forward_distance != 0u) {
			/*
			 * A stale/out-of-order record is invalid, not billions
			 * of missing records. The half-range comparison keeps
			 * normal uint32 sequence wraparound valid.
			 */
			++invalid_records_;
			return false;
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
	return true;
}

/*
 * Aggregate (MTR2) continuity: wire-sequence tracking only, on the
 * aggregate stream's own counter. A gap is counted, logged, and resynced.
 * There is deliberately NO sample-range check against the previous
 * aggregate: the PL enforces continuity of the 15 blocks INSIDE one
 * aggregate, but consecutive aggregates may legitimately be separated by
 * aggregation restarts (ineligible blocks, lock loss), which the sequence
 * gap already reports.
 */
bool MeterRecordIngestor::track_aggregate_continuity(
	const msap1::MeterRecord &record)
{
	if (!last_aggregate_sequence_)
		return true;
	const auto expected = *last_aggregate_sequence_ + 1u;
	const auto received = record.aggregate_sequence();
	const auto forward_distance = received - expected;
	if (forward_distance == 0u)
		return true;
	if (forward_distance < (std::uint32_t{1} << 31u)) {
		aggregate_sequence_gaps_ += forward_distance;
		log_message(dma_log, mnc::logging::Priority::warning,
			"aggregate record sequence gap: expected " +
				std::to_string(expected) + ", got " +
				std::to_string(received),
			"meter_aggregate_sequence_gap",
			{{"MNC_EXPECTED_SEQUENCE", std::to_string(expected)},
			 {"MNC_SEQUENCE", std::to_string(received)}});
		return true;
	}
	/* Stale/out-of-order, same half-range rule as the basic stream. */
	++invalid_records_;
	return false;
}

void MeterRecordIngestor::accept(const msap1::MeterRecord &record)
{
	if (!matches_configuration(record)) {
		++invalid_records_;
		return;
	}

	/*
	 * The stream interleaves two record formats with INDEPENDENT
	 * sequence counters, so continuity is tracked per format.
	 */
	const bool aggregate =
		record.record_format() == msap1::meter_aggregate_format;
	if (aggregate ? !track_aggregate_continuity(record)
		      : !track_basic_continuity(record))
		return;
	/*
	 * Decode-validate before publication: a record whose timing fields are
	 * malformed (zero-sample block, overflowing sample range, impossible
	 * cycle count) is invalid exactly like a failed configuration match —
	 * counted, logged, and never published or allowed to
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
	if (update.timing)
		stamp_time_state(*update.timing, timebase_);
	if (update.aggregate_timing)
		stamp_time_state(*update.aggregate_timing, timebase_);

	/* Durability barrier: latest-state publication is allowed only after the
	 * exact validated PL record has an ordered committed stream cursor. */
	mnc::meter_stream::MeterStreamRecord stream_record;
	stream_record.record_format = record.record_format();
	stream_record.record_kind = static_cast<std::uint16_t>(update.kind);
	stream_record.measurement_period = static_cast<std::uint8_t>(update.period);
	stream_record.source_sequence = update.sequence;
	stream_record.configuration_generation = update.configuration_generation;
	stream_record.ingested_at_nanoseconds =
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			received_at.time_since_epoch()).count();
	if (update.timing) {
		stream_record.timing.first_sample_index =
			update.timing->first_sample_index;
		stream_record.timing.sample_count = update.timing->sample_count;
		stream_record.timing.cycle_count = update.timing->cycle_count;
		stream_record.timing.time_quality =
			static_cast<std::uint8_t>(update.timing->time_quality);
		if (update.timing->utc_start)
			stream_record.timing.utc_start_nanoseconds =
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					update.timing->utc_start->time_since_epoch()).count();
		stream_record.timing.utc_uncertainty_nanoseconds =
			update.timing->utc_uncertainty_ns;
	} else if (update.aggregate_timing) {
		stream_record.timing.first_sample_index =
			update.aggregate_timing->first_sample_index;
		stream_record.timing.sample_count =
			update.aggregate_timing->sample_count;
		stream_record.timing.cycle_count =
			update.aggregate_timing->cycle_count;
		stream_record.timing.time_quality =
			static_cast<std::uint8_t>(update.aggregate_timing->time_quality);
		if (update.aggregate_timing->utc_start)
			stream_record.timing.utc_start_nanoseconds =
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					update.aggregate_timing->utc_start->time_since_epoch()).count();
		stream_record.timing.utc_uncertainty_nanoseconds =
			update.aggregate_timing->utc_uncertainty_ns;
	}
	const auto *bytes = reinterpret_cast<const std::byte *>(&record);
	stream_record.payload.assign(bytes, bytes + sizeof(record));
	(void)publisher_.publish(stream_record);
	meter_data_.apply(update);
	if (aggregate) {
		last_aggregate_sequence_ = record.aggregate_sequence();
		/* The aggregate cache is the 150/180-cycle counterpart of
		 * latest_record_: same raw wire form, own stream, own
		 * freshness clock. It never touches the basic caches. */
		latest_aggregate_record_ = record;
		/* Timing provenance is captured HERE, with the record, from
		 * the quality just stamped onto this aggregate's decoded
		 * timing. The raw PL record carries no UTC state, so anything
		 * that re-decodes the cached bytes later cannot recover it —
		 * and must never substitute the timebase's current quality,
		 * which describes a different moment. Unsynchronized is the
		 * conservative label if a future decoder ever omits the
		 * timing: never claim a synchronization that was not stamped. */
		latest_aggregate_time_quality_ =
			update.aggregate_timing
				? update.aggregate_timing->time_quality
				: msap1::meter::TimeQuality::Unsynchronized;
		last_aggregate_record_time_ = Clock::now();
	} else {
		/* Only basic records refresh the instantaneous-readings
		 * cache; aggregates are published through the typed store
		 * under MeasurementPeriod::Cycles150_180. */
		latest_record_ = record;
	}
	last_record_time_ = Clock::now();
	++meter_records_;
}

} // namespace msap1::acquisition::daemon
