#include "pipeline/record_ingestor.hpp"

#include "support/logs.hpp"

#include <chrono>
#include <cstdio>
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

std::string hex_word(std::uint32_t value)
{
	char buffer[11];
	(void)std::snprintf(buffer, sizeof buffer, "0x%08X", value);
	return buffer;
}

/* Words 0..15 cover the shared envelope (0..12) and the format-specific
 * header extension (13..15) and expose truncation and beat-shift patterns;
 * they are dumped RAW and unlabeled because words 13..15 depend on the
 * (possibly corrupted) format word, and a wrong label would poison exactly
 * the analysis this dump exists for. */
std::string header_words_hex(const msap1::MeterRecord &record)
{
	std::string result;
	for (std::size_t index = 0; index < 16; ++index) {
		if (index != 0)
			result += ' ';
		result += hex_word(record.word(index));
	}
	return result;
}

/* Words 9/10: the envelope first-sample index (every record type). Zero
 * here on a record whose front half is intact is the partial-emission
 * signature observed in the field. */
std::string sample_index_words_hex(const msap1::MeterRecord &record)
{
	return hex_word(record.word(9)) + ' ' + hex_word(record.word(10));
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
	last_ten_minute_sequence_.reset();
	last_two_hour_sequence_.reset();
	latest_aggregate_record_.reset();
	latest_aggregate_time_quality_ =
		msap1::meter::TimeQuality::Unsynchronized;
	last_record_time_.reset();
	last_aggregate_record_time_.reset();
	sequence_gaps_ = 0;
	aggregate_sequence_gaps_ = 0;
	ten_minute_sequence_gaps_ = 0;
	two_hour_sequence_gaps_ = 0;
}

void MeterRecordIngestor::clear_latest()
{
	/* A configuration swap is a deliberate boundary for BOTH record
	 * streams: the PL may reset either sequence while reconfiguring, and
	 * that must not be counted as packet loss. */
	latest_record_.reset();
	last_aggregate_sequence_.reset();
	last_ten_minute_sequence_.reset();
	last_two_hour_sequence_.reset();
	latest_aggregate_record_.reset();
	latest_aggregate_time_quality_ =
		msap1::meter::TimeQuality::Unsynchronized;
	last_record_time_.reset();
	last_aggregate_record_time_.reset();
}

bool MeterRecordIngestor::matches_configuration(
	const msap1::MeterRecord &record) const
{
	/* No window echo: a basic block is cycle-defined, so its word-6
	 * sample count legitimately varies with grid frequency and matches
	 * the configured value only in free-run. */
	return record.header_valid() &&
	       record.configuration_generation() ==
		       configuration_.wire.generation &&
	       record.sample_rate_hz() == configuration_.wire.sample_rate_hz;
}

/*
 * Basic (MTR1) continuity: wire-sequence tracking against the newest
 * accepted basic record, plus — for consecutive sequences — sample-range
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
			 * normal uint32 sequence wraparound valid. This is also
			 * where a partially-emitted record with a zeroed
			 * sequence word lands, so it dumps forensics like every
			 * other rejection.
			 */
			++invalid_records_;
			log_rejected_record(
				"stale or out-of-order basic sequence (expected " +
					std::to_string(expected) + ", received " +
					std::to_string(received) + ")",
				"meter_record_stale_rejected", record);
			return false;
		}
	}
	/*
	 * Sample-range continuity: blocks are gapless by construction on
	 * the PL conversion-domain counter, so between consecutive sequences
	 * first(N+1) must equal first(N) + count(N). A mismatch means lost
	 * samples that the sequence numbers did not reveal; count it as a gap
	 * and resync — this record still becomes the new continuity baseline,
	 * exactly like sequence-gap handling.
	 */
	if (sequence_continuous) {
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
	log_rejected_record(
		"stale or out-of-order aggregate sequence (expected " +
			std::to_string(expected) + ", received " +
			std::to_string(received) + ")",
		"meter_record_stale_rejected", record);
	return false;
}

/*
 * Clock-aligned ten-minute (M13) records are a third independent producer
 * stream. Its first record is normally sequence 1 even when hundreds or
 * thousands of basic records have already been accepted. Treating that
 * sequence as basic continuity rejects the valid boundary record as stale.
 *
 * Only the fundamental M13 record advances this baseline. Its power,
 * phasor, and unbalance siblings describe the same interval and deliberately
 * repeat the same sequence.
 */
bool MeterRecordIngestor::track_ten_minute_continuity(
	const msap1::MeterRecord &record)
{
	if (!last_ten_minute_sequence_)
		return true;
	const auto expected = *last_ten_minute_sequence_ + 1u;
	const auto received = record.sequence();
	const auto forward_distance = received - expected;
	if (forward_distance == 0u)
		return true;
	if (forward_distance < (std::uint32_t{1} << 31u)) {
		ten_minute_sequence_gaps_ += forward_distance;
		log_message(dma_log, mnc::logging::Priority::warning,
			"ten-minute record sequence gap: expected " +
				std::to_string(expected) + ", got " +
				std::to_string(received),
			"meter_ten_minute_sequence_gap",
			{{"MNC_EXPECTED_SEQUENCE", std::to_string(expected)},
			 {"MNC_SEQUENCE", std::to_string(received)}});
		return true;
	}
	++invalid_records_;
	log_rejected_record(
		"stale or out-of-order ten-minute sequence (expected " +
			std::to_string(expected) + ", received " +
			std::to_string(received) + ")",
		"meter_record_stale_rejected", record);
	return false;
}

/*
 * Two-hour (M14) records form a fourth independent stream. The fundamental
 * record alone advances the baseline; the power, phasor, and unbalance
 * siblings intentionally repeat its sequence because they describe the
 * same twelve-ten-minute interval.
 */
bool MeterRecordIngestor::track_two_hour_continuity(
	const msap1::MeterRecord &record)
{
	if (!last_two_hour_sequence_)
		return true;
	const auto expected = *last_two_hour_sequence_ + 1u;
	const auto received = record.sequence();
	const auto forward_distance = received - expected;
	if (forward_distance == 0u)
		return true;
	if (forward_distance < (std::uint32_t{1} << 31u)) {
		two_hour_sequence_gaps_ += forward_distance;
		log_message(dma_log, mnc::logging::Priority::warning,
			"two-hour record sequence gap: expected " +
				std::to_string(expected) + ", got " +
				std::to_string(received),
			"meter_two_hour_sequence_gap",
			{{"MNC_EXPECTED_SEQUENCE", std::to_string(expected)},
			 {"MNC_SEQUENCE", std::to_string(received)}});
		return true;
	}
	++invalid_records_;
	log_rejected_record(
		"stale or out-of-order two-hour sequence (expected " +
			std::to_string(expected) + ", received " +
			std::to_string(received) + ")",
		"meter_record_stale_rejected", record);
	return false;
}

/*
 * Field-incident forensics, covering EVERY silent rejection path. The
 * 2026-08-13..15 PL emission fault emits one partially-written record per
 * aggregate window; depending on where the truncation lands, the record
 * fails matches_configuration(), reads as a stale sequence (word 3 zeroed —
 * the common case, learned the hard way when instrumenting only the
 * configuration path caught nothing), or reaches the decoder. Every such
 * rejection dumps the raw words so the truncation-point distribution
 * localizes the failing PL stage.
 *
 * Rate limited to one entry per 2 s across all reasons: the observed fault
 * rejects one record per 3 s window (each passes), while a storm stays
 * bounded below the record rate. Suppressed rejections are counted and
 * reported on the next emitted entry, so the journal can never falsely
 * confirm a one-per-window pattern.
 */
void MeterRecordIngestor::log_rejected_record(const std::string &reason,
	const char *event, const msap1::MeterRecord &record)
{
	const auto now = Clock::now();
	if (last_reject_log_ &&
	    now - *last_reject_log_ < std::chrono::seconds(2)) {
		++suppressed_reject_logs_;
		return;
	}
	last_reject_log_ = now;
	const auto suppressed = suppressed_reject_logs_;
	suppressed_reject_logs_ = 0;

	const auto header_words = header_words_hex(record);
	const auto sample_index_words = sample_index_words_hex(record);
	log_message(dma_log, mnc::logging::Priority::warning,
		"meter record rejected: " + reason +
			"; seq=" + std::to_string(record.word(3)) +
			" words[0..15]=" + header_words +
			" words[9..10]=" + sample_index_words +
			(suppressed != 0
				 ? " (+" + std::to_string(suppressed) +
					   " rejections suppressed)"
				 : ""),
		event,
		{{"MNC_REJECT_REASON", reason},
		 {"MNC_SEQUENCE", std::to_string(record.word(3))},
		 {"MNC_RECORD_FORMAT", hex_word(record.word(1))},
		 {"MNC_CONFIGURATION_GENERATION",
		  std::to_string(record.word(4))},
		 {"MNC_EXPECTED_CONFIGURATION_GENERATION",
		  std::to_string(configuration_.wire.generation)},
		 {"MNC_SAMPLE_RATE_HZ", std::to_string(record.word(5))},
		 {"MNC_EXPECTED_SAMPLE_RATE_HZ",
		  std::to_string(configuration_.wire.sample_rate_hz)},
		 {"MNC_HEADER_WORDS", header_words},
		 {"MNC_SAMPLE_INDEX_WORDS", sample_index_words},
		 {"MNC_SUPPRESSED_REJECTS", std::to_string(suppressed)}});
}

/*
 * The reason re-derivation must stay in lockstep with
 * matches_configuration(): the final branch is only reachable for a record
 * failing the sample-rate check.
 */
void MeterRecordIngestor::log_configuration_mismatch(
	const msap1::MeterRecord &record)
{
	std::string reason;
	if (!record.header_valid())
		reason = "invalid header";
	else if (record.configuration_generation() !=
		 configuration_.wire.generation)
		reason = "generation mismatch (expected " +
			 std::to_string(configuration_.wire.generation) + ")";
	else
		reason = "sample rate mismatch (expected " +
			 std::to_string(configuration_.wire.sample_rate_hz) + ")";
	log_rejected_record(reason, "meter_record_config_rejected", record);
}

void MeterRecordIngestor::accept(const msap1::MeterRecord &record)
{
	if (!matches_configuration(record)) {
		++invalid_records_;
		log_configuration_mismatch(record);
		return;
	}

	/*
	 * Single-cycle diagnostic records (metrology M2) interleave on the
	 * same DMA stream with their own sequence space. They are accepted
	 * and counted here but neither continuity-tracked against the
	 * basic/aggregate counters nor published: host-side consumption is
	 * a later milestone. Falling through would poison the basic-record
	 * gap accounting at ~60 records/s.
	 */
	if (record.record_format() == msap1::meter_single_cycle_format) {
		++single_cycle_records_;
		latest_single_cycle_ = msap1::decode_single_cycle_record(record);
		return;
	}

	/*
	 * Power-quality records (metrology M12) arrive from a separate PL
	 * producer port onto the same DMA stream, with their own sequence
	 * space and their own cadence (a heartbeat every ~100 half cycles
	 * plus an edge record per event). Same rule as the single-cycle
	 * stream: counted and cached, never allowed near the basic or
	 * aggregate continuity trackers. Unlike the diagnostic stream this
	 * one is a product record, so a malformed one is a counted, logged
	 * fault rather than a silent decode.
	 */
	if (record.record_format() == msap1::meter_pq_event_format) {
		try {
			const auto snapshot =
				msap1::decode_pq_event_record(record);
			++pq_event_records_;
			latest_power_quality_ = snapshot;
			if (snapshot.values.kind !=
			    msap1::PowerQualityRecordKind::periodic) {
				latest_power_quality_event_ = snapshot;
				if (snapshot.values.kind ==
				    msap1::PowerQualityRecordKind::event_start)
					++pq_events_;
			}
		} catch (const std::exception &error) {
			++invalid_records_;
			log_rejected_record(
				std::string("power-quality record: ") +
					error.what(),
				"meter_record_decode_rejected", record);
		}
		return;
	}

	/*
	 * The stream interleaves record formats with INDEPENDENT sequence
	 * counters, so continuity is tracked per format. POWER and PHASOR
	 * records share their BASIC sibling's sequence by design (same
	 * block), so they must not touch the basic tracker: they would read
	 * as repeats and be dropped. The basic tracker already measures the
	 * shared transport's health.
	 */
	const bool sibling =
		record.record_format() == msap1::meter_power_format ||
		record.record_format() == msap1::meter_phasor_format ||
		record.record_format() == msap1::meter_unbalance_format ||
		record.record_format() == msap1::meter_aggregate_power_format ||
		record.record_format() == msap1::meter_aggregate_phasor_format ||
		record.record_format() ==
			msap1::meter_aggregate_unbalance_format ||
		record.record_format() == msap1::meter_ten_minute_power_format ||
		record.record_format() == msap1::meter_ten_minute_phasor_format ||
		record.record_format() ==
			msap1::meter_ten_minute_unbalance_format ||
		record.record_format() == msap1::meter_two_hour_power_format ||
		record.record_format() == msap1::meter_two_hour_phasor_format ||
		record.record_format() == msap1::meter_two_hour_unbalance_format;
	const bool aggregate =
		record.record_format() == msap1::meter_aggregate_format;
	const bool ten_minute =
		record.record_format() == msap1::meter_ten_minute_format;
	const bool two_hour =
		record.record_format() == msap1::meter_two_hour_format;
	if (!sibling) {
		const auto continuous =
			aggregate ? track_aggregate_continuity(record)
			: ten_minute ? track_ten_minute_continuity(record)
			: two_hour ? track_two_hour_continuity(record)
				     : track_basic_continuity(record);
		if (!continuous)
			return;
	}
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
		/* Same raw-word dump as the silent rejection paths: a decoder
		 * rejection is the partially-emitted record whose truncation
		 * landed past the sequence word, and its intact/zeroed word
		 * boundary is the forensic payload. Not rate limited — this
		 * path was always logged and is rare. */
		log_message(dma_log, mnc::logging::Priority::warning,
			"meter record rejected by decoder: " +
				std::string(error.what()) +
				"; words[0..15]=" + header_words_hex(record) +
				" words[9..10]=" + sample_index_words_hex(record),
			"meter_record_decode_rejected",
			{{"MNC_SEQUENCE", std::to_string(record.sequence())},
			 {"MNC_HEADER_WORDS", header_words_hex(record)},
			 {"MNC_SAMPLE_INDEX_WORDS",
			  sample_index_words_hex(record)}});
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
	} else if (ten_minute) {
		/* M13 has its own cadence and sequence space. It is retained by
		 * MeterData under MeasurementPeriod::Min10, but never replaces
		 * either the basic or 150/180-cycle raw-record cache. */
		last_ten_minute_sequence_ = record.sequence();
	} else if (two_hour) {
		/* M14 is retained under MeasurementPeriod::Hour2 and has its own
		 * producer sequence. Its four records must never replace a shorter
		 * tier's raw-record cache or continuity baseline. */
		last_two_hour_sequence_ = record.sequence();
	} else if (record.record_format() == msap1::meter_periodic_format) {
		/* ONLY the BASIC record refreshes the instantaneous-readings
		 * cache and the basic continuity baseline. Sibling records
		 * must not: the basic-period siblings share their BASIC
		 * record's sequence (harmless but redundant), while the
		 * AGGREGATE-period siblings carry the aggregate sequence —
		 * letting one become latest_record_ poisoned the basic
		 * baseline every 3 s and inflated sequence_gaps by a whole
		 * aggregate window (found on target the day M11 first ran). */
		latest_record_ = record;
	}
	last_record_time_ = Clock::now();
	++meter_records_;
}

} // namespace msap1::acquisition::daemon
