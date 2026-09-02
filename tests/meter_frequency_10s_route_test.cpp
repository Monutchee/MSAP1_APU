#include "meter_dto.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

#include <glaze/glaze.hpp>

namespace {

using Id = mnc::meter::MeterAttributeId;
using Key = mnc::meter::MeterAttributeKey;
using Quality = mnc::meter::ReadingQuality;
using Unit = mnc::meter::MeterUnit;
using msap1::web::api::meter_frequency_10s_dto;
using msap1::web::api::meter_frequency_10s_snapshot_selection;
using msap1::web::api::MeterFrequency10sUnavailableDto;

constexpr std::uint64_t first_sample = 9'007'199'254'740'000ull;
constexpr std::uint32_t sample_count = 1'280'000u;
constexpr std::uint64_t utc_start = 1'788'000'200'000'000'000ull;
constexpr std::uint64_t utc_end = utc_start + 10'000'000'000ull;

void require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

std::string json(const auto &value)
{
	auto text = glz::write_json(value);
	if (!text)
		throw std::runtime_error("failed to serialize frequency DTO");
	return *text;
}

msap1::MeterSnapshotResponse contract_response()
{
	msap1::MeterSnapshotResponse response{};
	response.running = true;
	response.has_snapshot = true;
	response.snapshot.period = mnc::meter::MeasurementPeriod::Seconds10;
	response.snapshot.sequence = 77;
	response.snapshot.configuration_generation = 12;
	response.snapshot.updated_at_nanoseconds =
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::system_clock::now().time_since_epoch() -
			std::chrono::milliseconds(1200)).count();
	mnc::meter::MeterSnapshotTiming timing{};
	timing.quality = mnc::meter::TimeQuality::Synchronized;
	timing.utc_start_nanoseconds = static_cast<std::int64_t>(utc_start);
	timing.utc_uncertainty_nanoseconds = 250;
	timing.first_sample_index = first_sample;
	timing.sample_count = sample_count;
	timing.sample_rate_hz = 128'000;
	timing.cycle_count = 500;
	timing.nominal_frequency_hz = 50;
	response.snapshot.timing = timing;
	const std::uint32_t source_status =
		(1u << 0u) | (1u << 1u) | (1u << 2u) | (1u << 3u) |
		(1u << 4u) | (1u << 9u) | (1u << 10u);
	const std::uint32_t status =
		msap1::meter_frequency_10s_status_result_valid |
		msap1::meter_frequency_10s_status_time_aligned |
		msap1::meter_frequency_10s_status_profile_supported |
		msap1::meter_frequency_10s_status_time_synchronized |
		msap1::meter_frequency_10s_status_filter_ready |
		msap1::meter_frequency_10s_status_reference_valid |
		msap1::meter_frequency_10s_status_calibration_valid |
		msap1::meter_frequency_10s_status_sample_rate_valid;
	response.snapshot.frequency_10s =
		mnc::meter::Frequency10sSnapshotMetadata{
			.interval_end_sample_index = first_sample + sample_count,
			.utc_start_nanoseconds = utc_start,
			.utc_end_nanoseconds = utc_end,
			.utc_uncertainty_nanoseconds = 250,
			.measured_sample_rate_millihz = 128'000'125,
			.source_sequence = 77,
			.boundary_generation = 9,
			.source_status = source_status,
			.status = status,
			.reasons = 0,
			.observer_drop_count = 0,
			.guard_flags = 0xcu,
			.observed_crossings = 501,
			.included_crossings = 501,
			.rejected_cycles = 0,
			.duration_q16_samples =
				static_cast<std::uint64_t>(sample_count) << 16u,
			.first_crossing_q16_samples = 100,
			.last_crossing_q16_samples =
				static_cast<std::int64_t>(
					static_cast<std::uint64_t>(sample_count) << 16u) +
				100,
			.nominal_frequency_hz = 50,
			.reference_channel = 6,
			.filter_profile = 1,
			.calibration_profile = 1,
		};
	response.snapshot.values.push_back({
		.attribute = Key{Id::Frequency, std::nullopt},
		.unit = Unit::MilliHertz,
		.quality = Quality::Valid,
		.value = 50'001,
		.source_sequence = 77,
		.measured_at_nanoseconds = static_cast<std::int64_t>(utc_end),
		.sample_count = sample_count,
		.calculation_window_nanoseconds = 10'000'000'000ll,
	});
	return response;
}

void selection_is_typed_and_minimal()
{
	const auto selection = meter_frequency_10s_snapshot_selection();
	require(selection.period == mnc::meter::MeasurementPeriod::Seconds10 &&
		selection.attributes ==
			(std::vector<Key>{{Id::Frequency, std::nullopt}}),
		"frequency endpoint did not select only the typed ten-second value");
}

void absence_has_the_stable_unavailable_shape()
{
	auto response = contract_response();
	response.has_snapshot = false;
	require(!meter_frequency_10s_dto(response),
		"missing ten-second result was rendered as available");
	response.has_snapshot = true;
	response.running = false;
	require(!meter_frequency_10s_dto(response),
		"stopped acquisition retained a ten-second result");
	require(json(MeterFrequency10sUnavailableDto{}) ==
		R"({"available":false})",
		"unavailable frequency JSON changed");
}

void valid_result_exposes_exact_value_and_audit_provenance()
{
	const auto projected = meter_frequency_10s_dto(contract_response());
	require(projected && projected->available && projected->valid &&
		projected->quality == "valid" && projected->frequency_hz == 50.001 &&
		projected->frequency_millihz == 50'001 &&
		projected->sequence == 77 &&
		projected->configuration_generation == 12 &&
		projected->time_quality == "synchronized" &&
		projected->clock_synchronized &&
		projected->class_a_time_qualified &&
		projected->first_sample_index == "9007199254740000" &&
		projected->interval_end_sample_index == "9007199256020000" &&
		projected->sample_count == sample_count &&
		projected->sample_rate_hz == 128'000 &&
		projected->measured_sample_rate_millihz == 128'000'125 &&
		projected->cycle_count == 500 &&
		projected->utc_start_nanoseconds == "1788000200000000000" &&
		projected->utc_end_nanoseconds == "1788000210000000000" &&
		projected->utc_uncertainty_nanoseconds == "250" &&
		projected->source_sequence == 77 &&
		projected->boundary_generation == 9 &&
		projected->nominal_frequency_hz == 50 &&
		projected->reference_channel == 6 &&
		projected->filter_profile == 1 &&
		projected->calibration_profile == 1,
		"valid ten-second frequency identity, value, or exact anchors changed");
	require(projected->status_flags ==
			(std::vector<std::string>{"result_valid", "time_aligned",
				"profile_supported", "time_synchronized", "filter_ready",
				"reference_valid", "calibration_valid",
				"sample_rate_valid"}) &&
		projected->rejection_reasons.empty() &&
		projected->source_status_flags ==
			(std::vector<std::string>{"boundary_valid",
				"time_synchronized", "sample_rate_valid", "filter_ready",
				"reference_valid", "calibration_valid",
				"profile_supported"}) &&
		projected->guard_flag_names ==
			(std::vector<std::string>{"exact_start", "exact_end"}),
		"ten-second frequency named audit flags changed");
	const auto body = json(*projected);
	require(body.find(R"("frequency_hz":50.001)") != std::string::npos &&
		body.find(R"("clock_synchronized":true)") != std::string::npos &&
		body.find(R"("class_a_time_qualified":true)") !=
			std::string::npos &&
		body.find(R"("first_sample_index":"9007199254740000")") !=
			std::string::npos &&
		body.find(R"("utc_end_nanoseconds":"1788000210000000000")") !=
			std::string::npos &&
		body.find(R"("rejection_reasons":[])") != std::string::npos,
		"ten-second frequency JSON loses value or exact audit integers");
}

void synchronized_clock_is_distinct_from_class_a_time_qualification()
{
	auto response = contract_response();
	auto &timing = *response.snapshot.timing;
	auto &audit = *response.snapshot.frequency_10s;
	auto &reading = response.snapshot.values.front();
	timing.quality = mnc::meter::TimeQuality::Unsynchronized;
	timing.utc_uncertainty_nanoseconds = 3'500'000u;
	audit.utc_uncertainty_nanoseconds = 3'500'000u;
	audit.reasons = msap1::meter_frequency_10s_reason_time_uncertainty;
	audit.status &= ~msap1::meter_frequency_10s_status_result_valid;
	reading.quality = Quality::Invalid;
	reading.value = 0;
	const auto projected = meter_frequency_10s_dto(response);
	require(projected && projected->clock_synchronized &&
		!projected->class_a_time_qualified &&
		projected->time_quality == "unsynchronized" &&
		projected->rejection_reasons ==
			(std::vector<std::string>{"time_uncertainty"}),
		"disciplined clock was conflated with Class A time qualification");
}

void invalid_result_never_publishes_zero_as_a_measurement()
{
	auto response = contract_response();
	auto &reading = response.snapshot.values.front();
	reading.quality = Quality::OutOfRange;
	reading.value = 0;
	auto &audit = *response.snapshot.frequency_10s;
	audit.reasons = msap1::meter_frequency_10s_reason_out_of_range;
	audit.status &= ~msap1::meter_frequency_10s_status_result_valid;
	audit.status |= msap1::meter_frequency_10s_status_out_of_range;
	const auto projected = meter_frequency_10s_dto(response);
	require(projected && !projected->valid &&
		projected->quality == "out_of_range" &&
		!projected->frequency_hz && !projected->frequency_millihz &&
		projected->rejection_reasons ==
			(std::vector<std::string>{"out_of_range"}) &&
		std::ranges::find(projected->status_flags, "out_of_range") !=
			projected->status_flags.end(),
		"invalid ten-second result was exposed as a numeric zero");
	const auto body = json(*projected);
	require(body.find(R"("frequency_hz")") == std::string::npos &&
		body.find(R"("frequency_millihz")") == std::string::npos,
		"invalid ten-second JSON did not omit both numeric values");
}

void malformed_snapshot_provenance_is_rejected()
{
	const auto rejects = [](msap1::MeterSnapshotResponse response) {
		try {
			(void)meter_frequency_10s_dto(response);
			return false;
		} catch (const std::invalid_argument &) {
			return true;
		}
	};
	auto wrong_period = contract_response();
	wrong_period.snapshot.period = mnc::meter::MeasurementPeriod::Basic;
	require(rejects(std::move(wrong_period)),
		"frequency projection accepted the wrong typed period");
	auto no_audit = contract_response();
	no_audit.snapshot.frequency_10s.reset();
	require(rejects(std::move(no_audit)),
		"frequency projection accepted missing audit metadata");
	auto wrong_utc = contract_response();
	wrong_utc.snapshot.frequency_10s->utc_end_nanoseconds += 1;
	require(rejects(std::move(wrong_utc)),
		"frequency projection accepted a non-ten-second UTC interval");
	auto wrong_sample = contract_response();
	wrong_sample.snapshot.frequency_10s->interval_end_sample_index += 1;
	require(rejects(std::move(wrong_sample)),
		"frequency projection accepted inconsistent sample anchors");
	auto wrong_sequence = contract_response();
	wrong_sequence.snapshot.values.front().source_sequence = 76;
	require(rejects(std::move(wrong_sequence)),
		"frequency projection accepted inconsistent source identity");
	auto unavailable = contract_response();
	unavailable.snapshot.values.front().quality = Quality::Unavailable;
	unavailable.snapshot.values.front().value = 0;
	unavailable.snapshot.frequency_10s->reasons =
		msap1::meter_frequency_10s_reason_filter_warmup;
	unavailable.snapshot.frequency_10s->status &=
		~msap1::meter_frequency_10s_status_result_valid;
	require(rejects(std::move(unavailable)),
		"frequency projection accepted provider-unavailable as a completed result");
}

} // namespace

int main()
{
	try {
		selection_is_typed_and_minimal();
		absence_has_the_stable_unavailable_shape();
		valid_result_exposes_exact_value_and_audit_provenance();
		synchronized_clock_is_distinct_from_class_a_time_qualification();
		invalid_result_never_publishes_zero_as_a_measurement();
		malformed_snapshot_provenance_is_rejected();
	} catch (const std::exception &error) {
		std::cerr << "meter frequency 10s route test failed: "
			  << error.what() << '\n';
		return 1;
	}
	return 0;
}
