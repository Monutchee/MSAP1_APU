#include "energy_dto.hpp"

#include <glaze/glaze.hpp>

#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

template<typename Unit>
void fill(msap1::PhaseABCTotal<msap1::Reading<Unit>> &group,
	std::int64_t start)
{
	group.phase_a.value = start;
	group.phase_b.value = start + 1;
	group.phase_c.value = start + 2;
	group.total.value = start + 3;
	group.phase_a.quality = group.phase_b.quality = group.phase_c.quality =
		group.total.quality = msap1::MeasurementQuality::valid;
}

} // namespace

int main()
{
	constexpr std::int64_t above_javascript_safe_integer = 9007199254740993LL;
	msap1::EnergyValues energy;
	fill(energy.active_import, above_javascript_safe_integer);
	fill(energy.active_export, above_javascript_safe_integer + 10);
	fill(energy.apparent, above_javascript_safe_integer + 20);
	for (std::size_t quadrant = 0; quadrant < 4; ++quadrant)
		fill(energy.reactive_quadrants[quadrant],
			above_javascript_safe_integer + 30 +
				static_cast<std::int64_t>(quadrant) * 10);
	energy.session_id = 0xfedcba9876543210ULL;
	energy.reset_epoch = 12;
	energy.discontinuity = true;
	const auto encoded = glz::write_json(msap1::web::api::energy_dto(energy));
	require(encoded.has_value(), "ENERGY DTO did not serialize");
	require(encoded->find("\"9007199254740993\"") != std::string::npos,
		"ENERGY counter crossed JSON as a number");
	require(encoded->find("\"18364758544493064720\"") != std::string::npos,
		"ENERGY session ID crossed JSON as a number");
	require(encoded->find("reactive_quadrant_iv_uvarh") != std::string::npos,
		"quadrant IV group is missing");
	require(encoded->find("\"discontinuity\":true") != std::string::npos,
		"ENERGY discontinuity metadata is missing");

	msap1::DemandValues demand;
	fill(demand.current_active, -above_javascript_safe_integer);
	fill(demand.import_peak, above_javascript_safe_integer);
	fill(demand.export_peak, above_javascript_safe_integer + 10);
	demand.session_id = energy.session_id;
	const auto demand_encoded = glz::write_json(
		msap1::web::api::demand_dto(demand));
	require(demand_encoded.has_value(), "DEMAND DTO did not serialize");
	require(demand_encoded->find("\"-9007199254740993\"") !=
			std::string::npos,
		"signed DEMAND crossed JSON as a number");
	return 0;
}
