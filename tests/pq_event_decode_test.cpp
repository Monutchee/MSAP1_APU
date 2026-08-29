/*
 * Pin the PQEVT-v1 power-quality decode against a synthetic record built
 * word-for-word from the PL contract (MSAP1_PL .../common/include/
 * measurement_record.hpp, metrology M12). A drift on either side of the
 * word map, the kind/flag packing in word 13, or the lane-to-phase
 * mapping of the valid mask fails here before it can reach the device.
 */

#include "msap1/meter/meter_data.hpp"
#include "msap1/meter/meter_record.hpp"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace {

int failures = 0;

void require(bool condition, const char *what)
{
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		++failures;
	}
}

/* An event-end record on phases A and C: interruption, grid locked,
 * armed, with distinct values in every decoded word. */
msap1::MeterRecord make_record()
{
	msap1::MeterRecord record{};
	record.words[0] = msap1::meter_record_magic;
	record.words[1] = msap1::meter_pq_event_format;
	record.words[2] = msap1::meter_record_size;
	record.words[3] = 7777;            /* PQ sequence space */
	record.words[4] = 0xCAFEF00D;      /* generation */
	record.words[5] = 32000;           /* sample rate */
	record.words[6] = 6400;            /* span sample count */
	record.words[7] = 0x7F;            /* every lane enabled */
	record.words[8] = 0;               /* status */
	record.words[9] = 0x11223344;      /* first sample low */
	record.words[10] = 0x00000002;     /* first sample high */
	/* kind = event_end(2), type = interruption(3), phases = A|C,
	 * locked (24), armed (26); fallback (25) clear. */
	record.words[13] = 2u | (3u << 8) | (0x5u << 16) | (1u << 24) |
			   (1u << 26);
	record.words[14] = 0x11224344;     /* last sample low */
	record.words[15] = 0x00000002;     /* last sample high */
	for (std::uint32_t phase = 0; phase < 3; ++phase) {
		record.words[16 + phase] = 230000000u + phase; /* Urms(1/2) */
		record.words[19 + phase] = 1000000u + phase;   /* window min */
		record.words[22 + phase] = 245000000u + phase; /* window max */
		record.words[25 + phase] = 5000000u + phase;   /* Irms(1/2) */
	}
	record.words[28] = 42;             /* event sequence */
	record.words[29] = 0x00000200;     /* duration low  (512) */
	record.words[30] = 0x00000001;     /* duration high (2^32) */
	record.words[31] = 20;             /* half-cycle updates */
	record.words[32] = 230000000u;     /* Udin, micro-volts */
	record.words[33] = 9000;           /* sag threshold, 1e-4 */
	record.words[34] = 11000;          /* swell threshold */
	record.words[35] = 1000;           /* interruption threshold */
	record.words[36] = 200;            /* hysteresis */
	return record;
}

msap1::MeterRecord make_lifecycle_record()
{
	msap1::MeterRecord record{};
	record.words[0] = msap1::meter_record_magic;
	record.words[1] = msap1::meter_pq_event_lifecycle_format;
	record.words[2] = msap1::meter_record_size;
	record.words[3] = 99;
	record.words[4] = 7;
	record.words[5] = 32000;
	record.words[6] = 1001;
	record.words[7] = 0x50; /* voltage phases A and C in the M18 map */
	record.words[8] = 0x0e; /* complete, first after gap, finalized */
	record.words[9] = 1000;
	record.words[12] = 2;
	record.words[13] = msap1::meter_event_lifecycle_update |
		(0u << 4u) | (0x5u << 8u);
	record.words[14] = 2000;
	record.words[16] = 0x55667788;
	record.words[17] = 0x11223344;
	record.words[18] = 42;
	record.words[20] = 7;
	record.words[21] = 9000;
	record.words[22] = 200;
	record.words[23] = 0x7u | (8u << 8u);
	record.words[24] = 100;
	record.words[25] = 500;
	record.words[26] = 230000000;
	for (std::uint32_t phase = 0; phase < 3; ++phase) {
		record.words[28 + phase] = 190000000 + phase;
		record.words[31 + phase] = 230000000 + phase;
		record.words[34 + phase] = 210000000 + phase;
	}
	record.words[37] = 1000;
	record.words[39] = 1100;
	record.words[45] = 0; /* R5 leaves UTC explicitly unresolved */
	record.words[46] = 2;
	record.words[47] = 4;
	record.words[48] = 0x11111111;
	record.words[49] = 0x22222222;
	record.words[50] = 0x33333333;
	record.words[51] = 0x44444444;
	return record;
}

} // namespace

int main()
{
	const auto record = make_record();
	require(record.header_valid(),
		"PQEVT-v1 must pass the shared header check");

	const auto snapshot = msap1::decode_pq_event_record(record);
	require(snapshot.sequence == 7777, "sequence");
	require(snapshot.configuration_generation == 0xCAFEF00D, "generation");
	require(snapshot.sample_rate_hz == 32000, "sample rate");
	require(snapshot.sample_count == 6400, "sample count");
	require(snapshot.valid_mask == 0x7F, "valid mask");
	require(snapshot.first_sample == 0x0000000211223344ull, "first sample");
	require(snapshot.last_sample == 0x0000000211224344ull, "last sample");
	require(!snapshot.first_after_gap() && !snapshot.arithmetic_error(),
		"clean status decodes without gap or arithmetic marks");

	const auto &values = snapshot.values;
	require(values.kind == msap1::PowerQualityRecordKind::event_end,
		"kind");
	require(values.event_type == msap1::PowerQualityEventType::interruption,
		"event type");
	require(values.affected_phases == 0x5u, "affected phase mask A|C");
	require(values.cycle_locked, "locked flag");
	require(!values.synthetic_half_cycle, "fallback flag clear");
	require(values.armed, "armed flag");
	require(values.event_sequence == 42, "event sequence");
	require(values.duration_samples == 0x0000000100000200ull,
		"64-bit duration in samples");
	require(values.half_cycle_updates == 20, "half-cycle updates");
	require(values.reference_micro_volts == 230000000u, "reference echo");
	require(values.sag_threshold_e4 == 9000 &&
			values.swell_threshold_e4 == 11000 &&
			values.interruption_threshold_e4 == 1000 &&
			values.hysteresis_e4 == 200,
		"threshold echo");

	/* Phase ordering: A/B/C read consecutive words from each base. */
	require(values.voltage.phase_a.value == 230000000 &&
			values.voltage.phase_b.value == 230000001 &&
			values.voltage.phase_c.value == 230000002,
		"Urms(1/2) per phase");
	require(values.voltage_minimum.phase_a.value == 1000000 &&
			values.voltage_minimum.phase_c.value == 1000002,
		"window minimum per phase");
	require(values.voltage_maximum.phase_a.value == 245000000 &&
			values.voltage_maximum.phase_c.value == 245000002,
		"window maximum per phase");
	require(values.current.phase_a.value == 5000000 &&
			values.current.phase_c.value == 5000002,
		"Irms(1/2) per phase");
	require(values.voltage.phase_a.valid() && values.current.phase_c.valid(),
		"an enabled lane decodes valid");
	require(values.voltage.phase_a.source_sequence == 7777,
		"readings carry the PQ sequence as provenance");
	require(values.voltage.phase_a.calculation_window.sample_count == 6400,
		"readings carry the record's span as their window");

	/* Lane mapping: Va/Vb/Vc are lanes 6/5/4 and Ia/Ib/Ic lanes 0/1/2, so
	 * a mask that keeps only Va and Ic must gate exactly those two. */
	{
		auto masked = record;
		masked.words[7] = 0x40u | 0x04u;
		const auto decoded = msap1::decode_pq_event_record(masked);
		require(decoded.values.voltage.phase_a.available() &&
				!decoded.values.voltage.phase_b.available() &&
				!decoded.values.voltage.phase_c.available(),
			"voltage lanes map Va/Vb/Vc to bits 6/5/4");
		require(!decoded.values.current.phase_a.available() &&
				!decoded.values.current.phase_b.available() &&
				decoded.values.current.phase_c.available(),
			"current lanes map Ia/Ib/Ic to bits 0/1/2");
		require(!decoded.values.voltage_minimum.phase_b.available(),
			"the mask gates the extremes with their lane");
	}

	/* A saturated accumulator (status bit 0) makes every root suspect
	 * without making it unavailable; bit 2 marks the span unchained. */
	{
		auto flagged = record;
		flagged.words[8] = 0x1u | 0x4u;
		const auto decoded = msap1::decode_pq_event_record(flagged);
		require(decoded.arithmetic_error() && decoded.first_after_gap(),
			"status bits 0 and 2 decode");
		require(decoded.values.voltage.phase_a.quality ==
				msap1::MeasurementQuality::arithmetic_error,
			"an arithmetic fault downgrades the readings");
	}

	/* A disarmed meter still measures: only detection is gated. */
	{
		auto disarmed = record;
		disarmed.words[13] = 0u;  /* periodic, no flags, not armed */
		disarmed.words[32] = 0;   /* reference 0 = disarmed */
		const auto decoded = msap1::decode_pq_event_record(disarmed);
		require(!decoded.values.armed, "reference 0 decodes as disarmed");
		require(decoded.values.kind ==
				msap1::PowerQualityRecordKind::periodic,
			"heartbeat kind");
		require(decoded.values.event_type ==
				msap1::PowerQualityEventType::none,
			"heartbeat carries no event type");
		require(decoded.values.voltage.phase_a.valid(),
			"a disarmed record still reports live Urms(1/2)");
	}

	/* Malformed records are rejected rather than half-decoded. */
	const auto rejects = [](const msap1::MeterRecord &bad, const char *what) {
		try {
			(void)msap1::decode_pq_event_record(bad);
		} catch (const std::invalid_argument &) {
			return;
		}
		std::fprintf(stderr, "FAIL: %s\n", what);
		++failures;
	};
	{
		auto bad = record;
		bad.words[1] = msap1::meter_periodic_format;
		rejects(bad, "a foreign format word must be rejected");
	}
	{
		auto bad = record;
		bad.words[6] = 0;
		rejects(bad, "a zero-sample span must be rejected");
	}
	{
		auto bad = record;
		bad.words[13] = 7u;  /* no such kind */
		rejects(bad, "an unknown kind must be rejected");
	}
	{
		auto bad = record;
		bad.words[14] = 0;
		bad.words[15] = 0;
		rejects(bad, "a last sample before the first must be rejected");
	}

	/* The M18 lifecycle record has an independent ID and sequence space and
	 * retains its exact settings snapshot through every lifecycle edge. */
	const auto lifecycle_record = make_lifecycle_record();
	require(lifecycle_record.header_valid(),
		"PQ-EVENT-v1 must pass the shared header check");
	const auto event =
		msap1::decode_pq_event_lifecycle_record(lifecycle_record);
	require(event.id.session == 0x1122334455667788ull &&
			event.id.counter == 42,
		"stable 128-bit event ID");
	require(event.lifecycle == msap1::PowerQualityEventLifecycle::update &&
			event.type == msap1::PowerQualityLifecycleType::voltage_sag,
		"lifecycle and taxonomy");
	require(event.phase_mask == 0x5 && event.valid_mask == 0x50,
		"M18 phase/validity map");
	require(event.first_sample == 1000 && event.last_sample == 2000 &&
			event.trigger_sample == 1100 && event.duration_samples == 1000,
		"exact event sample anchors");
	require(event.waveform_enabled && event.per_phase &&
			event.iec_classification && event.waveform_decimation == 8,
		"waveform/taxonomy settings snapshot");
	require(event.minimum_micro_units[0] == 190000000 &&
			event.maximum_micro_units[2] == 230000002 &&
			event.current_micro_units[1] == 210000001,
		"event extrema/current values");
	require(event.discontinuities == 2 && event.update_count == 4 &&
			event.time_quality == msap1::TimeQuality::Unsynchronized,
		"event discontinuity/update/time provenance");
	{
		auto bad = lifecycle_record;
		bad.words[52] = 1;
		rejects(bad, "PQ-EVENT-v1 reserved words must be zero");
	}
	{
		auto bad = lifecycle_record;
		bad.words[18] = 0;
		rejects(bad, "PQ-EVENT-v1 event counter must be nonzero");
	}
	{
		auto bad = lifecycle_record;
		bad.words[34] = 180000000;
		rejects(bad, "PQ-EVENT-v1 current value must lie within its extrema");
	}

	if (failures != 0) {
		std::fprintf(stderr, "FAILED: %d check(s)\n", failures);
		return EXIT_FAILURE;
	}
	std::printf("PASS: pq_event_decode_test\n");
	return EXIT_SUCCESS;
}
