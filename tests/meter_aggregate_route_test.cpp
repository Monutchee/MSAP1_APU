#include "meter_dto.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

#include <glaze/glaze.hpp>

/*
 * GET /api/v1/meter/aggregate contract tests.
 *
 * The route handler itself is a thin try/catch around meter_aggregate_dto(),
 * so the pinned JSON payload is tested here directly against the projection:
 * the absent shape, the exact field names and values of a decoded aggregate,
 * and the informative-only frequency semantics.
 */

namespace {

using msap1::web::api::meter_aggregate_dto;
using msap1::web::api::MeterAggregateUnavailableDto;

void require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

std::string json(const auto &value)
{
	auto text = glz::write_json(value);
	if (!text)
		throw std::runtime_error("failed to serialize the DTO");
	return *text;
}

/**
 * One MTR2 wire image using the values pinned in the REST contract: a 60 Hz
 * aggregate of 15 blocks / 180 cycles at 128 kSPS.
 */
msap1::MeterRecord contract_aggregate_record()
{
	msap1::MeterRecord record{};
	record.words[0] = msap1::meter_record_magic;
	record.words[1] = msap1::meter_aggregate_format;
	record.words[2] = msap1::meter_record_size;
	record.words[3] = 7;
	record.words[4] = 3'545'159'487u;
	record.words[5] = 128'000;
	record.words[6] = 384'015;
	/* Channels 0..6 contributed to every block; channel 7 (VCM) did not. */
	record.words[7] = 0x7f;
	/* complete | frequency_valid, no arithmetic error. */
	record.words[8] = (1u << 1) | (1u << 2);
	record.words[9] = 100;
	record.words[10] = 114;
	record.words[11] = 15u | (60u << 8) | (180u << 16);
	record.words[12] = 331'990'790u;
	record.words[13] = 0;
	/* Per-channel aggregate RMS, signed 64-bit micro-units at words
	 * 16..31: 1.5 A on every current, 120 V on every voltage. */
	const auto set_rms = [&record](std::size_t channel, std::int64_t micro) {
		const auto value = static_cast<std::uint64_t>(micro);
		record.words[16 + channel * 2] =
			static_cast<std::uint32_t>(value);
		record.words[17 + channel * 2] =
			static_cast<std::uint32_t>(value >> 32);
	};
	for (std::size_t channel = 0; channel < 4; ++channel)
		set_rms(channel, 1'500'000);
	for (std::size_t channel = 4; channel < 7; ++channel)
		set_rms(channel, 120'000'000);
	record.words[32] = 60'000;
	return record;
}

msap1::InfoResponse contract_response()
{
	msap1::InfoResponse response{};
	response.running = true;
	response.has_meter_record = true;
	response.has_aggregate_record = true;
	response.sample_rate_hz = 128'000;
	response.configuration_generation = 3'545'159'487u;
	response.meter_record_age_ms = 40;
	response.aggregate_record_age_ms = 1200;
	/* The daemon's CURRENT clock state, deliberately different from the
	 * aggregate's own provenance below: the payload must be built from
	 * the measurement's quality, so every test in this file fails if a
	 * refactor ever points the field back at this live value. */
	response.time_quality = msap1::meter::TimeQuality::Holdover;
	response.aggregate_time_quality = msap1::meter::TimeQuality::Synchronized;
	response.latest_aggregate_record = contract_aggregate_record();
	return response;
}

/*
 * Before the first aggregate exists — the first ~3 s after a start, or any
 * stretch of ineligible basic blocks — the endpoint reports absence rather
 * than failing, and nothing else is present in the body.
 */
void absence_renders_the_unavailable_shape()
{
	auto response = contract_response();
	response.has_aggregate_record = false;
	require(!meter_aggregate_dto(response).has_value(),
		"a missing aggregate was rendered as available");

	/* Capture stopped is the same story: the daemon answered, there is
	 * simply no current aggregate. */
	response.has_aggregate_record = true;
	response.running = false;
	require(!meter_aggregate_dto(response).has_value(),
		"a stopped pipeline was rendered as available");

	require(json(MeterAggregateUnavailableDto{}) ==
			R"({"available":false})",
		"the unavailable body is not exactly {\"available\":false}");
}

/*
 * The pinned payload, field name for field name. The frontend cross-checks
 * against this literal, so any rename fails here.
 */
void a_decoded_aggregate_matches_the_pinned_payload()
{
	const auto aggregate = meter_aggregate_dto(contract_response());
	require(aggregate.has_value(), "the aggregate was not rendered");

	require(aggregate->available && aggregate->sequence == 7 &&
		aggregate->configuration_generation == 3'545'159'487u &&
		aggregate->sample_rate_hz == 128'000 &&
		aggregate->sample_count == 384'015 &&
		aggregate->first_sample_index == 331'990'790u &&
		aggregate->first_basic_sequence == 100 &&
		aggregate->last_basic_sequence == 114 &&
		aggregate->basic_block_count == 15 &&
		aggregate->cycle_count == 180 &&
		aggregate->nominal_frequency_hz == 60 &&
		!aggregate->arithmetic_error &&
		aggregate->time_quality == "synchronized" &&
		aggregate->age_ms == 1200,
		"the aggregate identity was not projected as pinned");

	/* Channel order, naming, and units are the /meter/readings ones; the
	 * unmeasured VCM debug channel is invalid, never a valid zero. */
	require(aggregate->channels.size() == 8, "wrong channel count");
	require(aggregate->channels[0].index == 0 &&
		aggregate->channels[0].name == "ILA" &&
		aggregate->channels[0].unit == "A" &&
		aggregate->channels[0].valid &&
		aggregate->channels[0].rms == 1.5,
		"channel 0 was not projected as pinned");
	require(aggregate->channels[6].name == "VLA" &&
		aggregate->channels[6].unit == "V" &&
		aggregate->channels[6].valid &&
		aggregate->channels[6].rms == 120.0,
		"the voltage channel was not projected as pinned");
	require(aggregate->channels[7].name == "VCM" &&
		!aggregate->channels[7].valid &&
		aggregate->channels[7].rms == 0.0,
		"the unmeasured VCM channel was not reported invalid");

	const auto body = json(*aggregate);
	static constexpr const char *expected =
		R"({"available":true,"sequence":7,"configuration_generation":3545159487,)"
		R"("sample_rate_hz":128000,"sample_count":384015,)"
		R"("first_sample_index":331990790,"first_basic_sequence":100,)"
		R"("last_basic_sequence":114,"basic_block_count":15,)"
		R"("cycle_count":180,"nominal_frequency_hz":60,)"
		R"("arithmetic_error":false,"time_quality":"synchronized",)"
		R"("age_ms":1200,"channels":[)"
		R"({"index":0,"name":"ILA","unit":"A","valid":true,"rms":1.5},)"
		R"({"index":1,"name":"ILB","unit":"A","valid":true,"rms":1.5},)"
		R"({"index":2,"name":"ILC","unit":"A","valid":true,"rms":1.5},)"
		R"({"index":3,"name":"ILN","unit":"A","valid":true,"rms":1.5},)"
		R"({"index":4,"name":"VLC","unit":"V","valid":true,"rms":120},)"
		R"({"index":5,"name":"VLB","unit":"V","valid":true,"rms":120},)"
		R"({"index":6,"name":"VLA","unit":"V","valid":true,"rms":120},)"
		R"({"index":7,"name":"VCM","unit":"A","valid":false,"rms":0}],)"
		R"("frequency":{"millihz":60000,"informative":true}})";
	if (body != expected) {
		std::cerr << "expected: " << expected << "\n"
			  << "actual:   " << body << "\n";
		throw std::runtime_error(
			"the aggregate JSON does not match the pinned contract");
	}
}

/*
 * The aggregate frequency is INFORMATIVE ONLY: IEC 61000-4-30:2025 defines
 * the standardized frequency product over its own 10 s interval, which is
 * not implemented. The payload must therefore never carry a validity flag
 * for it, even though the PL's frequency_valid flag is set on this record.
 */
void the_frequency_is_informative_only()
{
	const auto aggregate = meter_aggregate_dto(contract_response());
	require(aggregate.has_value(), "the aggregate was not rendered");
	require(aggregate->frequency.millihz == 60'000 &&
		aggregate->frequency.informative,
		"the informative frequency was not carried through");
	const auto body = json(aggregate->frequency);
	require(body == R"({"millihz":60000,"informative":true})",
		"the frequency object gained a field it must not have");
}

/*
 * time_quality is the provenance of the MEASUREMENT, not of the HTTP
 * request. The daemon's live synchronization state may have moved many
 * times since the aggregate was ingested — an aggregate measured while
 * synchronized and read back during holdover must still report
 * "synchronized", and vice versa. The field name and its three values are
 * a committed frontend contract; only the source of the value is at stake.
 */
void the_time_quality_is_the_aggregates_not_the_daemons()
{
	using msap1::meter::TimeQuality;
	const auto rendered = [](TimeQuality aggregate, TimeQuality daemon) {
		auto response = contract_response();
		response.aggregate_time_quality = aggregate;
		response.time_quality = daemon;
		const auto dto = meter_aggregate_dto(response);
		require(dto.has_value(), "the aggregate was not rendered");
		return dto->time_quality;
	};

	/* Measured synchronized, read back after the clock degraded. */
	require(rendered(TimeQuality::Synchronized, TimeQuality::Holdover) ==
			"synchronized" &&
		rendered(TimeQuality::Synchronized,
			 TimeQuality::Unsynchronized) == "synchronized",
		"a synchronized aggregate was relabelled by the live clock");
	/* Measured during holdover or without UTC, read back after the clock
	 * recovered: recovery must not bless an older measurement. */
	require(rendered(TimeQuality::Holdover, TimeQuality::Synchronized) ==
			"holdover" &&
		rendered(TimeQuality::Unsynchronized,
			 TimeQuality::Synchronized) == "unsynchronized",
		"a degraded aggregate was blessed by the live clock");

	/* The three pinned JSON spellings, exactly as the frontend expects. */
	auto response = contract_response();
	response.aggregate_time_quality = TimeQuality::Holdover;
	const auto holdover = meter_aggregate_dto(response);
	require(holdover.has_value(), "the aggregate was not rendered");
	require(json(*holdover).find(R"("time_quality":"holdover")") !=
		std::string::npos,
		"the holdover spelling changed in the JSON body");
}

/*
 * Aggregation quality rules survive the projection: an arithmetic error
 * outranks the per-channel valid mask, so a saturated aggregate can never be
 * published as a valid reading.
 */
void an_arithmetic_error_invalidates_every_channel()
{
	auto response = contract_response();
	response.latest_aggregate_record.words[8] |= 1u;
	const auto aggregate = meter_aggregate_dto(response);
	require(aggregate.has_value(), "the aggregate was not rendered");
	require(aggregate->arithmetic_error,
		"the arithmetic-error flag was not projected");
	for (const auto &channel : aggregate->channels)
		require(!channel.valid && channel.rms == 0.0,
			"a saturated aggregate was published as valid");
}

/*
 * The projection decodes through the shared registry, so it inherits the
 * decoder's identity validation instead of trusting the cached bytes.
 */
void a_malformed_cached_record_is_rejected()
{
	auto response = contract_response();
	/* 15 blocks at a 60 Hz nominal is 180 cycles, never 150. */
	response.latest_aggregate_record.words[11] =
		15u | (60u << 8) | (150u << 16);
	try {
		(void)meter_aggregate_dto(response);
	} catch (const std::invalid_argument &) {
		return;
	}
	throw std::runtime_error(
		"a malformed cached aggregate was rendered as valid");
}

} // namespace

int main()
{
	try {
		absence_renders_the_unavailable_shape();
		a_decoded_aggregate_matches_the_pinned_payload();
		the_frequency_is_informative_only();
		the_time_quality_is_the_aggregates_not_the_daemons();
		an_arithmetic_error_invalidates_every_channel();
		a_malformed_cached_record_is_rejected();
	} catch (const std::exception &error) {
		std::cerr << "meter aggregate route test failed: "
			  << error.what() << "\n";
		return 1;
	}
	return 0;
}
