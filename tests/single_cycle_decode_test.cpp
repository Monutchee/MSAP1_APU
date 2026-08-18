/*
 * Pin the SCYC-v2 single-cycle diagnostic decode against a synthetic
 * record built word-for-word from the PL contract
 * (MSAP1_PL .../common/include/measurement_record.hpp). A drift on either
 * side of the word map fails here before it can reach the device.
 */

#include "msap1/meter/meter_data.hpp"
#include "msap1/meter/meter_record.hpp"

#include <cstdio>
#include <cstdlib>

namespace {

int failures = 0;

void require(bool condition, const char *what)
{
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		++failures;
	}
}

} // namespace

int main()
{
	msap1::MeterRecord record{};
	record.words[0] = msap1::meter_record_magic;
	record.words[1] = msap1::meter_single_cycle_format;
	record.words[2] = msap1::meter_record_size;
	record.words[3] = 4242;                 /* sequence */
	record.words[4] = 0xCAFEF00D;           /* generation */
	record.words[5] = 32000;                /* sample rate */
	record.words[6] = 534;                  /* sample count */
	record.words[7] = 0x7F;                 /* valid mask */
	record.words[8] = 0x1;                  /* status: arithmetic */
	record.words[9] = 0x11223344;           /* first sample low */
	record.words[10] = 0x00000002;          /* first sample high */
	record.words[13] = 60u | (1u << 8) | (0x5u << 16); /* timing */
	record.words[14] = 90001;               /* cycle sequence */
	record.words[16] = 0x55667788;          /* last sample low */
	record.words[17] = 0x00000002;          /* last sample high */
	record.words[18] = 0xAABBCCDD;          /* tick low */
	record.words[19] = 0x00000009;          /* tick high */
	record.words[20] = 60021;               /* frequency mHz */
	record.words[21] = 0x3;                 /* frequency status */
	for (std::size_t lane = 0; lane < 7; ++lane) {
		record.words[24 + lane * 2] = 1000000u + lane;
		record.words[24 + lane * 2 + 1] = lane;  /* high word */
	}
	for (std::size_t pair = 0; pair < 3; ++pair) {
		record.words[38 + pair * 2] = 2000000u + pair;
		record.words[38 + pair * 2 + 1] = 0;
	}
	/* Phase A: positive import; phase B: negative (export) as the
	 * sign-extension pin; phase C: zero. */
	record.words[44] = 360000000u;
	record.words[45] = 0;
	const std::uint64_t negative_bits =
		static_cast<std::uint64_t>(std::int64_t{-180000000});
	record.words[46] = static_cast<std::uint32_t>(negative_bits);
	record.words[47] = static_cast<std::uint32_t>(negative_bits >> 32);
	record.words[48] = 0;
	record.words[49] = 0;

	require(record.header_valid(),
		"SCYC-v2 must pass the shared header check");

	const auto snapshot = msap1::decode_single_cycle_record(record);
	require(snapshot.sequence == 4242, "sequence");
	require(snapshot.configuration_generation == 0xCAFEF00D, "generation");
	require(snapshot.sample_count == 534, "sample count");
	require(snapshot.valid_mask == 0x7F && snapshot.status == 0x1,
		"mask/status");
	require(snapshot.first_sample == 0x0000000211223344ull, "first sample");
	require(snapshot.last_sample == 0x0000000255667788ull, "last sample");
	require(snapshot.nominal_hz == 60 && snapshot.flags == 0x5,
		"timing word split");
	require(snapshot.cycle_sequence == 90001, "cycle sequence");
	require(snapshot.processing_tick == 0x00000009AABBCCDDull,
		"processing tick");
	require(snapshot.frequency_millihz == 60021 &&
			snapshot.frequency_status == 0x3,
		"frequency words");
	for (std::size_t lane = 0; lane < 7; ++lane)
		require(snapshot.rms_micro_units[lane] ==
				((std::uint64_t{lane} << 32) | (1000000u + lane)),
			"lane RMS");
	for (std::size_t pair = 0; pair < 3; ++pair)
		require(snapshot.vll_rms_micro_units[pair] == 2000000u + pair,
			"VLL RMS");
	require(snapshot.active_power_picowatts[0] == 360000000, "phase A P");
	require(snapshot.active_power_picowatts[1] == -180000000,
		"phase B P sign extension");
	require(snapshot.active_power_picowatts[2] == 0, "phase C P");

	if (failures != 0) {
		std::fprintf(stderr, "FAILED: %d check(s)\n", failures);
		return EXIT_FAILURE;
	}
	std::printf("PASS: single_cycle_decode_test\n");
	return EXIT_SUCCESS;
}
