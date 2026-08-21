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

	if (failures != 0) {
		std::fprintf(stderr, "FAILED: %d check(s)\n", failures);
		return EXIT_FAILURE;
	}
	std::printf("PASS: pq_event_decode_test\n");
	return EXIT_SUCCESS;
}
