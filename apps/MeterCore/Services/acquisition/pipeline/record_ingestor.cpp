#include "pipeline/record_ingestor.hpp"

#include "msap1/meter/MeterDataProvider/stream/meter_stream_ipc.hpp"
#include "support/logs.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace msap1::acquisition::daemon {
namespace {

constexpr std::size_t maximum_drain_batches_per_wake = 8;

std::size_t harmonic_period_index(msap1::MeasurementPeriod period)
{
	const auto index = static_cast<std::size_t>(period);
	if (index >= 4u)
		throw std::invalid_argument("unsupported harmonic measurement period");
	return index;
}

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
	mnc::meter_stream::MeterRecordPublisher &publisher,
	PqLifecycleCallback pq_lifecycle_callback)
	: meter_(meter), configuration_(configuration),
	  timebase_(timebase), publisher_(publisher),
	  pq_lifecycle_callback_(std::move(pq_lifecycle_callback))
{
}

void MeterRecordIngestor::read_available()
{
	for (std::size_t drain = 0; drain < maximum_drain_batches_per_wake;
	     ++drain) {
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
			note_invalid_record();
			log_message(dma_log, mnc::logging::Priority::warning,
				"meter DMA returned a partial record",
				"dma_partial_record",
				{{"MNC_DMA_BYTES", std::to_string(batch.bytes)}});
			return;
		}
		for (std::size_t index = 0; index < batch.count; ++index)
			accept(batch.records[index]);
	}
}

void MeterRecordIngestor::begin_epoch()
{
	invalid_records_ = 0;
	latest_record_.reset();
	last_aggregate_sequence_.reset();
	last_ten_minute_sequence_.reset();
	last_two_hour_sequence_.reset();
	last_frequency_10s_sequence_.reset();
	last_pq_lifecycle_sequence_.reset();
	latest_pq_lifecycle_.reset();
	last_flicker_sequence_.reset();
	for (auto &latest : latest_flicker_)
		latest.reset();
	last_mains_signal_sequence_.reset();
	latest_mains_signal_.reset();
	latest_aggregate_record_.reset();
	latest_aggregate_time_quality_ =
		msap1::meter::TimeQuality::Unsynchronized;
	last_record_time_.reset();
	last_aggregate_record_time_.reset();
	sequence_gaps_ = 0;
	aggregate_sequence_gaps_ = 0;
	ten_minute_sequence_gaps_ = 0;
	two_hour_sequence_gaps_ = 0;
	frequency_10s_sequence_gaps_ = 0;
	pq_lifecycle_sequence_gaps_ = 0;
	flicker_sequence_gaps_ = 0;
	mains_signal_sequence_gaps_ = 0;
	for (auto &assembler : harmonic_assemblers_)
		assembler.reset();
	for (auto &pending : pending_harmonic_families_)
		pending.reset();
	for (auto &latest : latest_harmonic_spectra_)
		latest.reset();
	energy_assembler_.reset();
	pending_energy_family_.reset();
	incomplete_energy_families_ = 0;
	incomplete_harmonic_families_ = 0;
	incomplete_harmonic_families_by_period_.fill(0);
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
	last_frequency_10s_sequence_.reset();
	last_pq_lifecycle_sequence_.reset();
	latest_pq_lifecycle_.reset();
	last_flicker_sequence_.reset();
	for (auto &latest : latest_flicker_)
		latest.reset();
	last_mains_signal_sequence_.reset();
	latest_mains_signal_.reset();
	latest_aggregate_record_.reset();
	latest_aggregate_time_quality_ =
		msap1::meter::TimeQuality::Unsynchronized;
	last_record_time_.reset();
	last_aggregate_record_time_.reset();
	for (auto &assembler : harmonic_assemblers_)
		assembler.reset();
	for (auto &pending : pending_harmonic_families_)
		pending.reset();
	for (auto &latest : latest_harmonic_spectra_)
		latest.reset();
	energy_assembler_.reset();
	pending_energy_family_.reset();
}

const std::optional<msap1::HarmonicSpectrumSnapshot> &
MeterRecordIngestor::latest_harmonic_spectrum(
	msap1::MeasurementPeriod period) const
{
	return latest_harmonic_spectra_[harmonic_period_index(period)];
}

std::uint64_t MeterRecordIngestor::harmonic_records(
	msap1::MeasurementPeriod period) const
{
	return harmonic_records_by_period_[harmonic_period_index(period)];
}

std::uint64_t MeterRecordIngestor::harmonic_families(
	msap1::MeasurementPeriod period) const
{
	return harmonic_families_by_period_[harmonic_period_index(period)];
}

std::uint64_t MeterRecordIngestor::incomplete_harmonic_families(
	msap1::MeasurementPeriod period) const
{
	return incomplete_harmonic_families_by_period_[
		harmonic_period_index(period)];
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
 * Basic 10/12-cycle continuity: wire-sequence tracking against the newest
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
			note_invalid_record();
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
			const auto first = record.first_sample_index();
			const auto count = record.block_sample_count();
			const auto previous_first =
				latest_record_->first_sample_index();
			const auto previous_last = expected_first - 1u;
			const bool range_safe =
				count != 0u && first <=
					std::numeric_limits<std::uint64_t>::max() -
						static_cast<std::uint64_t>(count - 1u);
			const auto last = range_safe
				? first + static_cast<std::uint64_t>(count) - 1u
				: 0u;
			const bool intentional_utc_overlap =
				record.timing().utc_resynchronized && range_safe &&
				first > previous_first && first <= previous_last &&
				last > previous_last;
			if (!intentional_utc_overlap) {
				++sequence_gaps_;
				log_message(dma_log, mnc::logging::Priority::warning,
					"meter record sample range is discontinuous: expected " +
						std::to_string(expected_first) + ", got " +
						std::to_string(first),
					"meter_sample_range_gap",
					{{"MNC_EXPECTED_SAMPLE_INDEX",
					  std::to_string(expected_first)},
					 {"MNC_FIRST_SAMPLE_INDEX",
					  std::to_string(first)}});
			}
		}
	}
	return true;
}

/*
 * Aggregate 150/180-cycle continuity: wire-sequence tracking only, on the
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
	note_invalid_record();
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
	note_invalid_record();
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
	note_invalid_record();
	log_rejected_record(
		"stale or out-of-order two-hour sequence (expected " +
			std::to_string(expected) + ", received " +
			std::to_string(received) + ")",
		"meter_record_stale_rejected", record);
	return false;
}

/* R5C1 owns the authoritative ten-second frequency sequence. It is wholly
 * independent of Basic and every aggregate tier; valid and explicitly
 * invalid placeholders both advance this baseline. */
bool MeterRecordIngestor::track_frequency_10s_continuity(
	const msap1::MeterRecord &record)
{
	if (!last_frequency_10s_sequence_)
		return true;
	const auto expected = *last_frequency_10s_sequence_ + 1u;
	const auto received = record.sequence();
	const auto forward_distance = received - expected;
	if (forward_distance == 0u)
		return true;
	if (forward_distance < (std::uint32_t{1} << 31u)) {
		frequency_10s_sequence_gaps_ += forward_distance;
		log_message(dma_log, mnc::logging::Priority::warning,
			"frequency 10-second record sequence gap: expected " +
				std::to_string(expected) + ", got " +
				std::to_string(received),
			"meter_frequency_10s_sequence_gap",
			{{"MNC_EXPECTED_SEQUENCE", std::to_string(expected)},
			 {"MNC_SEQUENCE", std::to_string(received)}});
		return true;
	}
	note_invalid_record();
	log_rejected_record(
		"stale or out-of-order frequency 10-second sequence (expected " +
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
 * Rate limited to one entry per 2 s within each interval category: the
 * observed fault rejects one record per 3 s window (each passes), while a
 * storm stays bounded below the record rate. Keeping independent limiters
 * ensures suppressed 10/12-cycle records can never be reported as belonging
 * to a visible 150/180-cycle, ten-minute, or two-hour warning.
 */
void MeterRecordIngestor::log_rejected_record(const std::string &reason,
	const char *event, const msap1::MeterRecord &record)
{
	const auto interval = record_interval_identity(record);
	auto &log_state = reject_log_states_[
		static_cast<std::size_t>(interval.category)];
	const auto now = Clock::now();
	if (log_state.last_log &&
	    now - *log_state.last_log < std::chrono::seconds(2)) {
		++log_state.suppressed;
		return;
	}
	log_state.last_log = now;
	const auto suppressed = log_state.suppressed;
	log_state.suppressed = 0;

	const auto header_words = header_words_hex(record);
	const auto sample_index_words = sample_index_words_hex(record);
	log_message(dma_log, mnc::logging::Priority::warning,
		"meter record rejected: interval=" +
			std::string(interval.label) + "; " + reason +
			"; seq=" + std::to_string(record.word(3)) +
			" words[0..15]=" + header_words +
			" words[9..10]=" + sample_index_words +
			(suppressed != 0
				 ? " (+" + std::to_string(suppressed) +
					   " same-interval rejections suppressed)"
				 : ""),
		event,
		{{"MNC_REJECT_REASON", reason},
		 {"MNC_INTERVAL_CATEGORY", std::string(interval.code)},
		 {"MNC_INTERVAL_LABEL", std::string(interval.label)},
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
		note_invalid_record();
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
			note_invalid_record();
			log_rejected_record(
				std::string("power-quality record: ") +
					error.what(),
				"meter_record_decode_rejected", record);
		}
		return;
	}

	/* PQ-EVENT-v1 is the final R5C1 lifecycle product. Decode it before
	 * touching this family's continuity baseline, stamp its first-sample UTC
	 * mapping into the durable envelope, and publish the exact 256-byte record.
	 * It never participates in the BASIC/aggregate sequence trackers. */
	if (record.record_format() == msap1::meter_pq_event_lifecycle_format) {
		msap1::PowerQualityEventLifecycleSnapshot event{};
		try {
			event = msap1::decode_pq_event_lifecycle_record(record);
		} catch (const std::exception &error) {
			note_invalid_record();
			log_rejected_record(
				std::string("PQ-EVENT-v1 record: ") + error.what(),
				"meter_record_decode_rejected", record);
			return;
		}
		if (last_pq_lifecycle_sequence_) {
			const auto delta = static_cast<std::int32_t>(
				event.sequence - (*last_pq_lifecycle_sequence_ + 1u));
			if (delta < 0)
				return;
			if (delta > 0)
				pq_lifecycle_sequence_gaps_ +=
					static_cast<std::uint32_t>(delta);
		}

		const auto received_at = std::chrono::system_clock::now();
		msap1::meter::BlockTiming timing{};
		timing.sequence = event.sequence;
		timing.configuration_generation = event.configuration_generation;
		timing.first_sample_index = event.first_sample;
		timing.sample_count = record.block_sample_count();
		timing.sample_rate_hz = event.sample_rate_hz;
		stamp_time_state(timing, timebase_);

		mnc::meter_stream::MeterStreamRecord stream_record{};
		stream_record.record_format = record.record_format();
		stream_record.record_kind = static_cast<std::uint16_t>(
			msap1::RecordKind::power_quality_event);
		stream_record.measurement_period = static_cast<std::uint8_t>(
			msap1::MeasurementPeriod::Basic);
		stream_record.source_sequence = event.sequence;
		stream_record.configuration_generation =
			event.configuration_generation;
		stream_record.ingested_at_nanoseconds =
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				received_at.time_since_epoch()).count();
		stream_record.timing.first_sample_index = event.first_sample;
		stream_record.timing.sample_count = record.block_sample_count();
		stream_record.timing.time_quality =
			static_cast<std::uint8_t>(timing.time_quality);
		if (timing.utc_start)
			stream_record.timing.utc_start_nanoseconds =
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					timing.utc_start->time_since_epoch()).count();
		stream_record.timing.utc_uncertainty_nanoseconds =
			timing.utc_uncertainty_ns;
		const auto *bytes = reinterpret_cast<const std::byte *>(&record);
		stream_record.payload.assign(bytes, bytes + sizeof(record));
		(void)publisher_.publish(stream_record);
		if (pq_lifecycle_callback_) {
			try {
				pq_lifecycle_callback_(event);
			} catch (const std::exception &error) {
				log_rejected_record(
					std::string("PQ event waveform trigger: ") +
						error.what(),
					"pq_event_waveform_trigger_failed", record);
			}
		}

		last_pq_lifecycle_sequence_ = event.sequence;
		latest_pq_lifecycle_ = std::move(event);
		++pq_lifecycle_records_;
		++meter_records_;
		last_record_time_ = Clock::now();
		return;
	}

	/* FLICKER-v1 has a private R5C1 sequence and three independently useful
	 * latest views. Validate the complete public record before advancing that
	 * sequence, then place the exact 256-byte source record in the durable
	 * stream before replacing its typed latest slot. */
	if (record.record_format() == msap1::meter_flicker_format) {
		msap1::FlickerSnapshot flicker{};
		try {
			flicker = msap1::decode_flicker_record(record);
		} catch (const std::exception &error) {
			note_invalid_record();
			log_rejected_record(
				std::string("FLICKER-v1 record: ") + error.what(),
				"meter_record_decode_rejected", record);
			return;
		}
		if (last_flicker_sequence_) {
			const auto delta = static_cast<std::int32_t>(
				flicker.sequence - (*last_flicker_sequence_ + 1u));
			if (delta < 0)
				return;
			if (delta > 0)
				flicker_sequence_gaps_ +=
					static_cast<std::uint32_t>(delta);
		}

		const auto received_at = std::chrono::system_clock::now();
		msap1::meter::BlockTiming timing{};
		timing.sequence = flicker.sequence;
		timing.configuration_generation =
			flicker.configuration_generation;
		timing.first_sample_index = flicker.first_sample;
		timing.sample_count = flicker.sample_count;
		timing.sample_rate_hz = flicker.sample_rate_hz;
		stamp_time_state(timing, timebase_);

		mnc::meter_stream::MeterStreamRecord stream_record{};
		stream_record.record_format = record.record_format();
		stream_record.record_kind = static_cast<std::uint16_t>(
			msap1::RecordKind::flicker);
		const auto period = flicker.kind == msap1::FlickerRecordKind::live
			? msap1::MeasurementPeriod::Basic
			: flicker.kind == msap1::FlickerRecordKind::pst
				? msap1::MeasurementPeriod::Min10
				: msap1::MeasurementPeriod::Hour2;
		stream_record.measurement_period = static_cast<std::uint8_t>(period);
		stream_record.source_sequence = flicker.sequence;
		stream_record.configuration_generation =
			flicker.configuration_generation;
		stream_record.ingested_at_nanoseconds =
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				received_at.time_since_epoch()).count();
		stream_record.timing.first_sample_index = flicker.first_sample;
		stream_record.timing.sample_count = flicker.sample_count;
		stream_record.timing.time_quality =
			static_cast<std::uint8_t>(timing.time_quality);
		if (timing.utc_start)
			stream_record.timing.utc_start_nanoseconds =
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					timing.utc_start->time_since_epoch()).count();
		stream_record.timing.utc_uncertainty_nanoseconds =
			timing.utc_uncertainty_ns;
		const auto *bytes = reinterpret_cast<const std::byte *>(&record);
		stream_record.payload.assign(bytes, bytes + sizeof(record));
		(void)publisher_.publish(stream_record);

		last_flicker_sequence_ = flicker.sequence;
		latest_flicker_[static_cast<std::size_t>(flicker.kind)] = flicker;
		++flicker_records_;
		++meter_records_;
		last_record_time_ = Clock::now();
		return;
	}

	/* MAINS-SIGNAL-v1 is one strict 200 ms observation per record. Its
	 * producer sequence is independent of every other family; malformed or
	 * stale observations are quarantined before the durable/latest boundary. */
	if (record.record_format() == msap1::meter_mains_signal_format) {
		msap1::MainsSignalSnapshot mains{};
		try {
			mains = msap1::decode_mains_signal_record(record);
		} catch (const std::exception &error) {
			note_invalid_record();
			log_rejected_record(
				std::string("MAINS-SIGNAL-v1 record: ") + error.what(),
				"meter_record_decode_rejected", record);
			return;
		}
		if (last_mains_signal_sequence_) {
			const auto delta = static_cast<std::int32_t>(
				mains.sequence - (*last_mains_signal_sequence_ + 1u));
			if (delta < 0)
				return;
			if (delta > 0)
				mains_signal_sequence_gaps_ +=
					static_cast<std::uint32_t>(delta);
		}

		const auto received_at = std::chrono::system_clock::now();
		msap1::meter::BlockTiming timing{};
		timing.sequence = mains.sequence;
		timing.configuration_generation = mains.configuration_generation;
		timing.first_sample_index = mains.first_sample;
		timing.sample_count = mains.sample_count;
		timing.sample_rate_hz = mains.sample_rate_hz;
		stamp_time_state(timing, timebase_);

		mnc::meter_stream::MeterStreamRecord stream_record{};
		stream_record.record_format = record.record_format();
		stream_record.record_kind = static_cast<std::uint16_t>(
			msap1::RecordKind::mains_signal);
		stream_record.measurement_period = static_cast<std::uint8_t>(
			msap1::MeasurementPeriod::Basic);
		stream_record.source_sequence = mains.sequence;
		stream_record.configuration_generation =
			mains.configuration_generation;
		stream_record.ingested_at_nanoseconds =
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				received_at.time_since_epoch()).count();
		stream_record.timing.first_sample_index = mains.first_sample;
		stream_record.timing.sample_count = mains.sample_count;
		stream_record.timing.time_quality =
			static_cast<std::uint8_t>(timing.time_quality);
		if (timing.utc_start)
			stream_record.timing.utc_start_nanoseconds =
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					timing.utc_start->time_since_epoch()).count();
		stream_record.timing.utc_uncertainty_nanoseconds =
			timing.utc_uncertainty_ns;
		const auto *bytes = reinterpret_cast<const std::byte *>(&record);
		stream_record.payload.assign(bytes, bytes + sizeof(record));
		(void)publisher_.publish(stream_record);

		last_mains_signal_sequence_ = mains.sequence;
		latest_mains_signal_ = std::move(mains);
		++mains_signal_records_;
		++meter_records_;
		last_record_time_ = Clock::now();
		return;
	}

	/*
	 * HARMONIC-v1 is a product family on its own sequence space. Decode and
	 * family-validate all 42 chunks first, then commit those exact records in
	 * one IPC request and one spool transaction. A partial family can neither
	 * enter the durable stream nor replace the previous complete snapshot.
	 */
	if (record.record_format() == msap1::meter_harmonic_format ||
	    record.record_format() == msap1::meter_harmonic_aggregate_format) {
		msap1::HarmonicRecordChunk chunk{};
		msap1::HarmonicAssemblyUpdate assembly{};
		try {
			chunk = msap1::decode_harmonic_record(record);
			assembly = harmonic_assemblers_[
				harmonic_period_index(chunk.period)].accept(chunk);
		} catch (const std::exception &error) {
			note_invalid_record();
			log_rejected_record(
				std::string("harmonic record: ") + error.what(),
				"meter_record_decode_rejected", record);
			return;
		}

		const auto received_at = std::chrono::system_clock::now();
		msap1::meter::BlockTiming timing{};
		timing.sequence = chunk.sequence;
		timing.configuration_generation = chunk.configuration_generation;
		timing.first_sample_index = chunk.first_sample;
		timing.sample_count = chunk.sample_count;
		timing.sample_rate_hz = chunk.sample_rate_hz;
		timing.cycle_count = chunk.cycle_count;
		timing.nominal_frequency = chunk.nominal_frequency_hz == 50u
			? msap1::meter::NominalFrequency::Hz50
			: msap1::meter::NominalFrequency::Hz60;
		timing.cycle_locked = (chunk.status & 0x4u) != 0u;
		stamp_time_state(timing, timebase_);

		mnc::meter_stream::MeterStreamRecord stream_record{};
		stream_record.record_format = record.record_format();
		stream_record.record_kind = static_cast<std::uint16_t>(
			msap1::RecordKind::harmonic);
		stream_record.measurement_period =
			static_cast<std::uint8_t>(chunk.period);
		stream_record.source_sequence = chunk.sequence;
		stream_record.source_fragment = static_cast<std::uint16_t>(
			chunk.channel * msap1::harmonic_chunks_per_channel +
			chunk.chunk);
		stream_record.configuration_generation =
			chunk.configuration_generation;
		stream_record.ingested_at_nanoseconds =
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				received_at.time_since_epoch()).count();
		stream_record.timing.first_sample_index = chunk.first_sample;
		stream_record.timing.sample_count = chunk.sample_count;
		stream_record.timing.cycle_count = chunk.cycle_count;
		stream_record.timing.time_quality =
			static_cast<std::uint8_t>(timing.time_quality);
		if (timing.utc_start)
			stream_record.timing.utc_start_nanoseconds =
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					timing.utc_start->time_since_epoch()).count();
		stream_record.timing.utc_uncertainty_nanoseconds =
			timing.utc_uncertainty_ns;
		const auto *bytes = reinterpret_cast<const std::byte *>(&record);
		stream_record.payload.assign(bytes, bytes + sizeof(record));

		const auto period_index = harmonic_period_index(chunk.period);
		auto &pending_family = pending_harmonic_families_[period_index];
		if (!pending_family ||
		    pending_family->sequence != chunk.sequence) {
			pending_family.emplace();
			pending_family->sequence = chunk.sequence;
		}
		const auto record_index = static_cast<std::size_t>(
			stream_record.source_fragment);
		if (pending_family->records[record_index])
			throw std::logic_error(
				"harmonic publication buffer received a duplicate chunk");
		pending_family->records[record_index] =
			std::move(stream_record);
		++pending_family->count;

		++harmonic_records_;
		++harmonic_records_by_period_[period_index];
		incomplete_harmonic_families_ += assembly.incomplete_families;
		incomplete_harmonic_families_by_period_[period_index] +=
			assembly.incomplete_families;
		if (assembly.completed) {
			if (pending_family->count !=
			    msap1::harmonic_records_per_family)
				throw std::logic_error(
					"complete harmonic family has a partial publication buffer");
			std::vector<mnc::meter_stream::MeterStreamRecord> family;
			family.reserve(msap1::harmonic_records_per_family);
			for (auto &pending : pending_family->records) {
				if (!pending)
					throw std::logic_error(
						"complete harmonic family is missing a publication chunk");
				family.push_back(std::move(*pending));
			}
			const auto cursors = publisher_.publish_records(family);
			if (cursors.size() != family.size())
				throw std::runtime_error(
					"harmonic family publish returned the wrong cursor count");
			latest_harmonic_spectra_[period_index] =
				std::move(*assembly.completed);
			++harmonic_families_;
			++harmonic_families_by_period_[period_index];
			pending_family.reset();
		}
		++meter_records_;
		return;
	}

	/* ENERGY-v1 is cumulative but is not publishable record-by-record. Buffer
	 * both parts, validate their complete identity, commit the pair in one
	 * meter-stream transaction, and only then replace the Basic latest view. */
	if (record.record_format() == msap1::meter_energy_format) {
		msap1::EnergyFamilyIdentity identity{};
		msap1::EnergyAssemblyUpdate assembly{};
		try {
			identity = msap1::decode_energy_identity(record);
			if (pending_energy_family_ &&
			    pending_energy_family_->identity != identity)
				pending_energy_family_.reset();
			if (!pending_energy_family_) {
				pending_energy_family_.emplace();
				pending_energy_family_->identity = identity;
			}
			const auto part = static_cast<std::size_t>(record.energy_part());
			auto &pending = pending_energy_family_->records[part];
			const auto *bytes =
				reinterpret_cast<const std::byte *>(&record);
			if (pending) {
				if (pending->payload.size() != sizeof(record) ||
				    !std::equal(bytes, bytes + sizeof(record),
					pending->payload.begin()))
					throw std::invalid_argument(
						"ENERGY duplicate part has different payload");
			} else {
				mnc::meter_stream::MeterStreamRecord stream_record{};
				stream_record.record_format = record.record_format();
				stream_record.record_kind = static_cast<std::uint16_t>(
					msap1::RecordKind::energy);
				stream_record.measurement_period = static_cast<std::uint8_t>(
					msap1::MeasurementPeriod::Basic);
				stream_record.source_sequence = identity.sequence;
				stream_record.source_fragment =
					static_cast<std::uint16_t>(part);
				stream_record.configuration_generation =
					identity.configuration_generation;
				stream_record.ingested_at_nanoseconds =
					std::chrono::duration_cast<std::chrono::nanoseconds>(
						std::chrono::system_clock::now().time_since_epoch()).count();
				stream_record.timing.first_sample_index =
					identity.first_sample_index;
				stream_record.timing.sample_count = identity.sample_count;
				stream_record.payload.assign(bytes, bytes + sizeof(record));
				pending = std::move(stream_record);
				++pending_energy_family_->count;
			}
			assembly = energy_assembler_.accept(record);
		} catch (const std::exception &error) {
			note_invalid_record();
			log_rejected_record(
				std::string("energy record: ") + error.what(),
				"meter_record_decode_rejected", record);
			return;
		}

		++energy_records_;
		incomplete_energy_families_ += assembly.incomplete_families;
		if (assembly.completed) {
			if (!pending_energy_family_ ||
			    pending_energy_family_->count != 2u ||
			    !pending_energy_family_->records[0] ||
			    !pending_energy_family_->records[1])
				throw std::logic_error(
					"complete ENERGY family has a partial publication buffer");

			msap1::meter::BlockTiming timing{};
			timing.sequence = identity.sequence;
			timing.configuration_generation =
				identity.configuration_generation;
			timing.first_sample_index = identity.first_sample_index;
			timing.sample_count = identity.sample_count;
			timing.sample_rate_hz = identity.sample_rate_hz;
			stamp_time_state(timing, timebase_);
			for (auto &pending : pending_energy_family_->records) {
				pending->timing.time_quality =
					static_cast<std::uint8_t>(timing.time_quality);
				if (timing.utc_start)
					pending->timing.utc_start_nanoseconds =
						std::chrono::duration_cast<std::chrono::nanoseconds>(
							timing.utc_start->time_since_epoch()).count();
				pending->timing.utc_uncertainty_nanoseconds =
					timing.utc_uncertainty_ns;
			}
			std::array<mnc::meter_stream::MeterStreamRecord, 2> family{
				std::move(*pending_energy_family_->records[0]),
				std::move(*pending_energy_family_->records[1])};
			std::vector<std::uint64_t> cursors;
			try {
				cursors = publisher_.publish_records(family);
			} catch (const msap1::energy_ledger::Conflict &error) {
				/* A repeated R5 boot nonce can make a fresh volatile
				 * counter stream look stale to the lifetime ledger. Preserve
				 * the authoritative checkpoint and quarantine this family,
				 * but never let one M17 product take down voltage/current/
				 * power acquisition. A later family with a fresh session ID
				 * is accepted without operator intervention. */
				note_invalid_record();
				++incomplete_energy_families_;
				log_rejected_record(
					std::string("ENERGY ledger conflict: ") +
						error.what(),
					"meter_energy_ledger_conflict", record);
				pending_energy_family_.reset();
				return;
			}
			if (cursors.size() != family.size())
				throw std::runtime_error(
					"ENERGY family publish returned the wrong cursor count");

			msap1::MeterUpdate update{};
			update.period = msap1::MeasurementPeriod::Basic;
			update.kind = msap1::RecordKind::energy;
			update.sequence = identity.sequence;
			update.configuration_generation =
				identity.configuration_generation;
			update.energy = std::move(*assembly.completed);
			if (auto *authority = dynamic_cast<
				msap1::meter_stream::EnergyAuthority *>(&publisher_)) {
				auto authoritative = authority->energy();
				if (!authoritative)
					throw std::runtime_error(
						"meter-stream acknowledged ENERGY without a durable snapshot");
				update.energy = std::move(*authoritative);
			}
			update.timing = timing;
			meter_data_.apply(update);
			++energy_families_;
			pending_energy_family_.reset();
		}
		++meter_records_;
		last_record_time_ = Clock::now();
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
		record.record_format() == msap1::meter_demand_format ||
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
		record.record_format() == msap1::meter_two_hour_unbalance_format ||
		record.record_format() == msap1::meter_ten_minute_open_power_format ||
		record.record_format() == msap1::meter_ten_minute_open_phasor_format ||
		record.record_format() == msap1::meter_ten_minute_open_unbalance_format ||
		record.record_format() == msap1::meter_two_hour_open_power_format ||
		record.record_format() == msap1::meter_two_hour_open_phasor_format ||
		record.record_format() == msap1::meter_two_hour_open_unbalance_format;
	const bool aggregate =
		record.record_format() == msap1::meter_aggregate_format;
	const bool ten_minute =
		record.record_format() == msap1::meter_ten_minute_format;
	const bool two_hour =
		record.record_format() == msap1::meter_two_hour_format;
	const bool frequency_10s =
		record.record_format() == msap1::meter_frequency_10s_format;
	const bool open_preview =
		record.record_format() == msap1::meter_ten_minute_open_format ||
		record.record_format() == msap1::meter_two_hour_open_format;
	/* Preview sequences are diagnostic and intentionally lossy. They have
	 * independent producer spaces, and a missing preview never means the
	 * authoritative completed stream lost a result. */
	if (!sibling && !open_preview) {
		const auto continuous =
			frequency_10s ? track_frequency_10s_continuity(record)
			: aggregate ? track_aggregate_continuity(record)
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
		note_invalid_record();
		const auto interval = record_interval_identity(record);
		/* Same raw-word dump as the silent rejection paths: a decoder
		 * rejection is the partially-emitted record whose truncation
		 * landed past the sequence word, and its intact/zeroed word
		 * boundary is the forensic payload. Not rate limited — this
		 * path was always logged and is rare. */
		log_message(dma_log, mnc::logging::Priority::warning,
			"meter record rejected by decoder: interval=" +
				std::string(interval.label) + "; " +
				std::string(error.what()) +
				"; words[0..15]=" + header_words_hex(record) +
				" words[9..10]=" + sample_index_words_hex(record),
			"meter_record_decode_rejected",
			{{"MNC_REJECT_REASON", error.what()},
			 {"MNC_INTERVAL_CATEGORY", std::string(interval.code)},
			 {"MNC_INTERVAL_LABEL", std::string(interval.label)},
			 {"MNC_SEQUENCE", std::to_string(record.sequence())},
			 {"MNC_RECORD_FORMAT", hex_word(record.record_format())},
			 {"MNC_HEADER_WORDS", header_words_hex(record)},
			 {"MNC_SAMPLE_INDEX_WORDS",
			  sample_index_words_hex(record)}});
		return;
	}
	/* FREQUENCY-10S already carries its normative UTC endpoints. Restamping
	 * it from the timebase at ingest time would rewrite the measurement. */
	if (update.timing && !frequency_10s)
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
	try {
		(void)publisher_.publish(stream_record);
	} catch (const msap1::energy_ledger::Conflict &error) {
		if (!update.demand)
			throw;
		/* DEMAND shares the R5 session identity with ENERGY. Reject a
		 * colliding/stale interval without advancing peaks, while leaving
		 * every other record stream operational until a fresh R5 session
		 * arrives. */
		note_invalid_record();
		log_rejected_record(
			std::string("DEMAND ledger conflict: ") + error.what(),
			"meter_demand_ledger_conflict", record);
		return;
	}
	if (update.demand) {
		if (auto *authority = dynamic_cast<
			msap1::meter_stream::EnergyAuthority *>(&publisher_)) {
			auto authoritative = authority->demand();
			if (!authoritative)
				throw std::runtime_error(
					"meter-stream acknowledged DEMAND without a durable snapshot");
			update.demand = std::move(*authoritative);
		}
	}
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
	} else if (frequency_10s) {
		last_frequency_10s_sequence_ = record.sequence();
	} else if (!open_preview &&
		   record.record_format() == msap1::meter_periodic_format) {
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
