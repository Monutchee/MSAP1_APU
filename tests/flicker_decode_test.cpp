#include "msap1/meter/meter_data.hpp"
#include "msap1/meter/meter_record.hpp"

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

msap1::MeterRecord make_record(msap1::FlickerRecordKind kind)
{
	msap1::MeterRecord record{};
	record.words[0] = msap1::meter_record_magic;
	record.words[1] = msap1::meter_flicker_format;
	record.words[2] = msap1::meter_record_size;
	record.words[3] = 42u;
	record.words[4] = 7u;
	record.words[5] = 32000u;
	const auto interval = kind == msap1::FlickerRecordKind::live ? 1u :
		kind == msap1::FlickerRecordKind::pst ? 600u : 7200u;
	const auto valid = interval * 2000u;
	record.words[6] = interval * record.words[5];
	record.words[7] = 0x70u;
	record.words[8] = 0x4u;
	write_u64(record, 9u, 1000u);
	record.words[13] = static_cast<std::uint32_t>(kind) | (0x7u << 8u);
	write_u64(record, 14u,
		1000u + static_cast<std::uint64_t>(interval) * record.words[5] - 1u);
	for (std::size_t phase = 0u; phase < 3u; ++phase) {
		record.words[16u + phase] = static_cast<std::uint32_t>(
			(phase + 1u) << 16u);
		record.words[25u + phase] = valid;
		if (kind != msap1::FlickerRecordKind::live)
			record.words[19u + phase] = static_cast<std::uint32_t>(
				(phase + 2u) << 16u);
		if (kind == msap1::FlickerRecordKind::plt)
			record.words[22u + phase] = static_cast<std::uint32_t>(
				(phase + 3u) << 16u);
	}
	record.words[28] = interval;
	record.words[29] = 7u;
	record.words[30] = 120u | (60u << 16u);
	record.words[31] = 0x3u;
	write_u64(record, 32u, 1000u);
	return record;
}

void require_rejected(msap1::MeterRecord record, const char *message)
{
	try {
		(void)msap1::decode_flicker_record(record);
		require(false, message);
	} catch (const std::invalid_argument &) {
	}
}

} // namespace

int main()
{
	for (const auto kind : {msap1::FlickerRecordKind::live,
		msap1::FlickerRecordKind::pst, msap1::FlickerRecordKind::plt}) {
		const auto decoded = msap1::decode_flicker_record(make_record(kind));
		require(decoded.kind == kind && decoded.sequence == 42u &&
			decoded.configuration_generation == 7u &&
			decoded.phase_valid_mask == 0x7u,
			"FLICKER-v1 identity/provenance");
		require(decoded.lamp_voltage == 120u &&
			decoded.nominal_frequency_hz == 60u &&
			decoded.first_after_gap() && !decoded.arithmetic_error(),
			"FLICKER-v1 model/status");
	}

	auto bad = make_record(msap1::FlickerRecordKind::live);
	bad.words[14] -= 1u;
	require_rejected(bad, "short live span accepted");
	bad = make_record(msap1::FlickerRecordKind::pst);
	bad.words[25] -= 1u;
	require_rejected(bad, "short phase-valid Pst count accepted");
	bad = make_record(msap1::FlickerRecordKind::pst);
	bad.words[22] = 1u;
	require_rejected(bad, "Pst carrying Plt accepted");
	bad = make_record(msap1::FlickerRecordKind::plt);
	bad.words[63] = 1u;
	require_rejected(bad, "reserved FLICKER-v1 word accepted");
	bad = make_record(msap1::FlickerRecordKind::live);
	bad.words[30] = 208u | (60u << 16u);
	require_rejected(bad, "unsupported lamp model accepted");

	if (failures != 0)
		return EXIT_FAILURE;
	std::printf("PASS: flicker_decode_test\n");
	return EXIT_SUCCESS;
}
