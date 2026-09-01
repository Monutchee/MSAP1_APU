#include "harmonic_dto.hpp"

#include <array>
#include <cmath>
#include <stdexcept>
#include <string>
#include <variant>

#include <glaze/glaze.hpp>

namespace {

void require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

msap1::HarmonicResponse response()
{
	msap1::HarmonicResponse response{};
	response.running = true;
	response.period = mnc::meter::MeasurementPeriod::Basic;
	response.records = 42;
	response.families = 1;
	response.has_snapshot = true;
	auto &snapshot = response.snapshot;
	snapshot.period = response.period;
	snapshot.sequence = 17;
	snapshot.configuration_generation = 9;
	snapshot.sample_rate_hz = 128'000;
	snapshot.sample_count = 25'600;
	snapshot.first_sample = 900'000;
	snapshot.status = 0x3eu;
	snapshot.valid_mask = 0x7fu;
	snapshot.qualified_max_order = 127;
	for (auto &channel : snapshot.channels) {
		for (std::size_t index = 0; index < channel.size(); ++index)
			channel[index].order = static_cast<std::uint8_t>(index + 1u);
		channel[0].magnitude_micro_units = 100'000'000u;
		for (std::size_t order = 1; order <= 50; ++order)
			channel[order - 1u].magnitude_valid = true;
	}
	/* Ia: sqrt(3^2 + 4^2) / 100 = 5%; Va: 10%. */
	snapshot.channels[0][2].magnitude_micro_units = 3'000'000u;
	snapshot.channels[0][4].magnitude_micro_units = 4'000'000u;
	snapshot.channels[6][2].magnitude_micro_units = 6'000'000u;
	snapshot.channels[6][4].magnitude_micro_units = 8'000'000u;
	return response;
}

} // namespace

int main()
{
	using msap1::web::api::harmonic_distortion_status_name;
	using Status = msap1::HarmonicDistortionStatus;
	const std::array statuses{
		std::pair{Status::valid, "valid"},
		std::pair{Status::interval_invalid, "interval_invalid"},
		std::pair{Status::channel_unavailable, "channel_unavailable"},
		std::pair{Status::fundamental_unavailable,
			  "fundamental_unavailable"},
		std::pair{Status::insufficient_order_range,
			  "insufficient_order_range"},
		std::pair{Status::harmonic_unavailable, "harmonic_unavailable"},
	};
	for (const auto &[status, name] : statuses)
		require(std::string{harmonic_distortion_status_name(status)} == name,
			"a public THD status spelling changed");

	auto projected = msap1::web::api::harmonic_dto(response());
	require(projected.available && projected.period == "basic" &&
		projected.configuration_generation == 9 &&
		projected.first_sample == 900'000 &&
		projected.sample_count == 25'600,
		"the harmonic interval identity was not preserved");
	require(projected.channels.size() == 7 &&
		projected.channels[0].name == "Ia" &&
		projected.channels[0].unit == "A" &&
		projected.channels[0].thd.status == "valid" &&
		std::holds_alternative<double>(projected.channels[0].thd.percent) &&
		std::abs(std::get<double>(projected.channels[0].thd.percent) - 5.0) <
			1e-12,
		"current-channel THD was mapped incorrectly");
	require(projected.channels[6].name == "Va" &&
		projected.channels[6].unit == "V" &&
		projected.channels[6].thd.status == "valid" &&
		std::holds_alternative<double>(projected.channels[6].thd.percent) &&
		std::abs(std::get<double>(projected.channels[6].thd.percent) - 10.0) <
			1e-12,
		"voltage-channel THD was mapped incorrectly");

	auto encoded = glz::write_json(projected);
	require(encoded.has_value(), "harmonic DTO did not serialize");
	require(encoded->find(
		R"("thd":{"percent":5,"first_order":2,"last_order":50,"status":"valid"})") !=
		std::string::npos,
		"the additive THD JSON shape changed");

	auto limited = response();
	limited.snapshot.qualified_max_order = 49;
	projected = msap1::web::api::harmonic_dto(limited);
	require(std::holds_alternative<std::nullptr_t>(
			projected.channels[0].thd.percent) &&
		projected.channels[0].thd.status == "insufficient_order_range",
		"rate-limited THD did not remain unavailable");
	encoded = glz::write_json(projected);
	require(encoded && encoded->find(
		R"("thd":{"percent":null,"first_order":2,"last_order":50,"status":"insufficient_order_range"})") !=
		std::string::npos,
		"an unavailable THD percentage is not serialized as null");

	auto unavailable = response();
	unavailable.has_snapshot = false;
	projected = msap1::web::api::harmonic_dto(unavailable);
	require(!projected.available && projected.channels.empty(),
		"an absent family unexpectedly exposed channels");
	return 0;
}
