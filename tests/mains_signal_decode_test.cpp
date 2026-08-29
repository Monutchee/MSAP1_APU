#include "msap1/meter/meter_data.hpp"
#include "msap1/meter/meter_record.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace {

int failures = 0;

void require(bool condition, const char *message)
{
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		++failures;
	}
}

void write_u64(msap1::MeterRecord &record, std::size_t word,
	std::uint64_t value)
{
	record.words[word] = static_cast<std::uint32_t>(value);
	record.words[word + 1u] = static_cast<std::uint32_t>(value >> 32u);
}

msap1::MeterRecord make_record(std::uint32_t rate = 32000u)
{
	msap1::MeterRecord record{};
	record.words[0] = msap1::meter_record_magic;
	record.words[1] = msap1::meter_mains_signal_format;
	record.words[2] = msap1::meter_record_size;
	record.words[3] = 42u;
	record.words[4] = 7u;
	record.words[5] = rate;
	record.words[6] = rate / 5u;
	record.words[7] = 0x70u;
	record.words[8] = 0x4u;
	write_u64(record, 9u, 1000u);
	record.words[13] = 0x507u;
	write_u64(record, 14u, 1000u + rate / 5u - 1u);
	record.words[16] = 500000u;
	record.words[17] = 505000u;
	record.words[18] = 1200000u;
	record.words[19] = 100000u;
	record.words[20] = 700000u;
	record.words[21] = 50000u;
	record.words[22] = 50000u;
	record.words[23] = 50000u;
	record.words[24] = 20000u;
	record.words[25] = 200u;
	record.words[26] = 7u;
	record.words[27] = 0x3u;
	record.words[28] = 50u;
	record.words[29] = 120000000u;
	return record;
}

void require_rejected(msap1::MeterRecord record, const char *message)
{
	try {
		(void)msap1::decode_mains_signal_record(record);
		require(false, message);
	} catch (const std::invalid_argument &) {
	}
}

} // namespace

int main()
{
	constexpr std::array supported_rates{
		2000u, 4000u, 8000u, 16000u, 32000u, 64000u, 128000u};
	for (const auto rate : supported_rates) {
		const auto decoded = msap1::decode_mains_signal_record(
			make_record(rate));
		require(decoded.sequence == 42u &&
			decoded.configuration_generation == 7u &&
			decoded.profile_generation == 7u &&
			decoded.sample_rate_hz == rate &&
			decoded.sample_count == rate / 5u,
			"MAINS-SIGNAL-v1 identity/provenance");
		require(decoded.phase_valid_mask == 0x7u &&
			decoded.detected_phase_mask == 0x5u &&
			decoded.any_detected() && decoded.first_after_gap() &&
			!decoded.arithmetic_error(),
			"MAINS-SIGNAL-v1 phase/status decode");
		require(decoded.configured_millihz == 500000u &&
			decoded.measured_millihz == 505000u &&
			decoded.bandwidth_millihz == 20000u &&
			decoded.observation_ms == 200u,
			"MAINS-SIGNAL-v1 estimator fields");
	}

	auto idle = make_record();
	idle.words[13] = 0x007u;
	idle.words[17] = idle.words[16];
	idle.words[18] = 100000u;
	idle.words[20] = 100000u;
	const auto idle_decoded = msap1::decode_mains_signal_record(idle);
	require(!idle_decoded.any_detected() &&
		idle_decoded.measured_millihz == idle_decoded.configured_millihz,
		"undetected carrier preserves configured frequency");

	auto bad = make_record();
	bad.words[14] -= 1u;
	require_rejected(bad, "short observation span accepted");
	bad = make_record();
	bad.words[13] = 0x707u;
	require_rejected(bad, "threshold-inconsistent phase detection accepted");
	bad = make_record();
	bad.words[17] = 520000u;
	require_rejected(bad, "out-of-band measured frequency accepted");
	bad = make_record();
	bad.words[27] |= 1u << 4u;
	require_rejected(bad, "hidden source arithmetic fault accepted");
	bad = make_record();
	bad.words[21] = 1300000u;
	require_rejected(bad, "hidden background-dominant status accepted");
	bad = make_record();
	bad.words[63] = 1u;
	require_rejected(bad, "reserved MAINS-SIGNAL-v1 word accepted");

	if (failures != 0)
		return EXIT_FAILURE;
	std::printf("PASS: mains_signal_decode_test\n");
	return EXIT_SUCCESS;
}
