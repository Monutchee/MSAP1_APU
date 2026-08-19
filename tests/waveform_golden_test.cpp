/*
 * Host test for the ADC-simulator golden model and the simulator wire
 * conversion: the two halves of the M0 verification framework. The wire
 * conversion (engineering units -> counts) and the golden expectation
 * (engineering units -> expected readings) must stay consistent with
 * each other and with the PL contract.
 */

#include "support/waveform_golden.hpp"

#include "msap1/meter/meter_config.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int failures = 0;

void require(bool condition, const std::string &what)
{
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", what.c_str());
		++failures;
	}
}

bool near(double value, double expected, double tolerance)
{
	return std::fabs(value - expected) <= tolerance;
}

msap1::MeterConversionFile simulator_profile()
{
	msap1::MeterConversionFile source;
	source.schema_version = 3;
	source.profile_id = "golden-test";
	source.adc_source = "simulator";
	source.nominal_frequency_hz = 60;
	source.adc_reference_volts = 1.0;
	/* Internal-CT current channels 0-3 and voltage dividers 4-6, matching
	 * the factory profile's shape (values chosen for round scales); the
	 * conversion authority requires channels 0 through 6 configured. */
	for (std::uint32_t index = 0; index < 4; ++index) {
		msap1::CurrentChannelConfig current;
		current.channel = index;
		current.name = "I" + std::to_string(index);
		current.enabled = true;
		current.sensor_model = "internal_ct";
		current.primary_rated_amps = 100.0;
		current.secondary_rated_amps = 0.05;
		current.burden_ohms = 10.0;
		source.current_channels.push_back(current);
	}
	for (std::uint32_t index = 4; index < 7; ++index) {
		msap1::VoltageChannelConfig voltage;
		voltage.channel = index;
		voltage.name = "V" + std::to_string(index);
		voltage.enabled = true;
		voltage.rin_ohms = 6000000.0;
		voltage.rf_ohms = 4640.0;
		source.voltage_channels.push_back(voltage);
	}
	return source;
}

void golden_expectation_composition()
{
	msap1::SimulatorConfig config;
	config.frequency_hz = 50.0;
	config.channels = {
		/* channel, rms, phase, dc, noise_rms */
		{0u, 10.0, -60.0, 0.0, 0.0},
		{6u, 120.0, 0.0, 0.0, 0.0},
		{1u, 0.0, 0.0, 2.5, 0.0},   /* dc only */
		{2u, 3.0, 0.0, 4.0, 0.0},   /* sine + dc */
		{3u, 0.0, 0.0, 0.0, 0.05},  /* noise only */
		{4u, 100.0, 0.0, 0.0, 1.0}, /* sine + noise */
	};
	const auto expectation = msap1::test::golden_expectation(config);
	require(expectation.frequency_hz == 50.0, "frequency passthrough");
	require(near(expectation.channels[0].ac_rms, 10.0, 1e-12),
		"pure sine ac RMS is the configured RMS");
	require(near(expectation.channels[0].total_rms, 10.0, 1e-12),
		"pure sine total RMS is the configured RMS");
	require(near(expectation.channels[1].mean, 2.5, 1e-12) &&
			near(expectation.channels[1].ac_rms, 0.0, 1e-12) &&
			near(expectation.channels[1].total_rms, 2.5, 1e-12),
		"dc-only channel: mean=dc, ac 0, total dc");
	require(near(expectation.channels[2].total_rms, 5.0, 1e-12),
		"3-4-5: sine 3 + dc 4 gives total RMS 5");
	require(near(expectation.channels[3].ac_rms, 0.05, 1e-12),
		"noise-only channel ac RMS is the noise RMS");
	require(near(expectation.channels[4].ac_rms,
			std::sqrt(100.0 * 100.0 + 1.0), 1e-12),
		"sine and noise combine in quadrature");
	require(!expectation.channels[5].enabled,
		"unconfigured channel stays disabled");

	/* Power: phase A pairs Va (lane 6, 120 V @ 0 deg) with Ia (lane 0,
	 * 10 A @ -60 deg): P = 1200 * cos(60 deg) = 600 W (the handover's
	 * own worked example). */
	require(near(expectation.power[0].active_power_watts, 600.0, 1e-9),
		"phase A active power via displacement angle");
	require(near(expectation.power[0].apparent_power_va, 1200.0, 1e-9),
		"phase A apparent power is Vrms x Irms");
	require(near(expectation.power[0].power_factor, 0.5, 1e-12),
		"phase A true PF is P / S");
}

void true_pf_differs_from_displacement_under_distortion()
{
	/* 10% 3rd harmonic on the voltage only: P keeps its fundamental
	 * value (no matching current harmonic), S grows with the total
	 * RMS, so the TRUE PF drops below the displacement factor -- the
	 * property that distinguishes it. */
	msap1::SimulatorConfig config;
	config.frequency_hz = 60.0;
	config.channels = {
		{0u, 10.0, 0.0, 0.0, 0.0},
		{1u, 0.0, 0.0, 0.0, 0.0},
		{2u, 0.0, 0.0, 0.0, 0.0},
		{3u, 0.0, 0.0, 0.0, 0.0},
		{4u, 0.0, 0.0, 0.0, 0.0},
		{5u, 0.0, 0.0, 0.0, 0.0},
		{6u, 120.0, 0.0, 0.0, 0.0},
	};
	config.harmonics = {{3u, 10.0, 0.0, "voltage"}};
	const auto expectation = msap1::test::golden_expectation(config);
	const double displacement = 1.0;  /* aligned phases */
	require(expectation.power[0].power_factor < displacement &&
			near(expectation.power[0].power_factor,
			     1.0 / std::sqrt(1.01), 1e-9),
		"true PF sinks with voltage distortion while displacement stays 1");
	require(near(expectation.power[0].active_power_watts, 1200.0, 1e-9),
		"voltage-only distortion adds no mean power");
	/* M9: the displacement quantities are fundamental-only, so the
	 * injected harmonic leaves every one of them untouched. */
	require(near(expectation.power[0].displacement_power_factor, 1.0, 1e-12),
		"displacement PF ignores distortion");
	require(near(expectation.power[0].fundamental_active_watts, 1200.0,
		     1e-9) &&
			near(expectation.power[0].reactive_power_vars, 0.0,
			     1e-9),
		"fundamental P1/Q1 ignore distortion");
}

void reactive_sign_follows_lag()
{
	/* Ia lags Va by 30 degrees: Q1 positive (inductive), phi1 = +30.
	 * Ib LEADS Vb by 90: Q1 = -S1 exactly, dPF 0. */
	msap1::SimulatorConfig config;
	config.frequency_hz = 60.0;
	config.channels = {
		{0u, 10.0, -30.0, 0.0, 0.0}, {1u, 5.0, -30.0, 0.0, 0.0},
		{2u, 0.0, 0.0, 0.0, 0.0},    {3u, 0.0, 0.0, 0.0, 0.0},
		{4u, 0.0, 0.0, 0.0, 0.0},    {5u, 120.0, -120.0, 0.0, 0.0},
		{6u, 120.0, 0.0, 0.0, 0.0},
	};
	const auto expectation = msap1::test::golden_expectation(config);
	require(near(expectation.power[0].reactive_power_vars,
		     1200.0 * 0.5, 1e-9) &&
			near(expectation.power[0].displacement_angle_degrees,
			     30.0, 1e-12),
		"a 30-degree lag gives Q1 = S1*sin(30) positive");
	require(near(expectation.power[0].displacement_power_factor,
		     std::cos(30.0 * M_PI / 180.0), 1e-12),
		"displacement PF is cos(phi1)");
	/* Phase B: Vb at -120, Ib at -30 -> phi1 = -90 (current leads). */
	require(near(expectation.power[1].reactive_power_vars, -600.0, 1e-9) &&
			near(expectation.power[1].displacement_angle_degrees,
			     -90.0, 1e-12) &&
			near(expectation.power[1].displacement_power_factor,
			     0.0, 1e-9),
		"a quadrature lead gives Q1 = -S1 and dPF 0");
}

void harmonic_expectation_composition()
{
	msap1::SimulatorConfig config;
	config.frequency_hz = 60.0;
	config.channels = {
		{0u, 10.0, -60.0, 0.0, 0.0},
		{1u, 0.0, 0.0, 0.0, 0.0},
		{2u, 0.0, 0.0, 0.0, 0.0},
		{3u, 0.0, 0.0, 0.0, 0.0},
		{4u, 0.0, 0.0, 0.0, 0.0},
		{5u, 0.0, 0.0, 0.0, 0.0},
		{6u, 120.0, 0.0, 0.0, 0.0},
	};
	/* 5% 3rd + 3% 5th on the voltage lanes only. */
	config.harmonics = {{3u, 5.0, 0.0, "voltage"},
			    {5u, 3.0, 0.0, "voltage"}};
	auto expectation = msap1::test::golden_expectation(config);
	require(near(expectation.channels[6].fundamental_rms, 120.0, 1e-12),
		"fundamental RMS ignores injected harmonics");
	require(near(expectation.channels[6].ac_rms,
			120.0 * std::sqrt(1.0 + 0.05 * 0.05 + 0.03 * 0.03),
			1e-9),
		"voltage total RMS grows by the harmonic energy");
	require(near(expectation.channels[0].ac_rms, 10.0, 1e-12),
		"current lane is untouched by voltage-only harmonics");
	require(near(expectation.power[0].active_power_watts,
			1200.0 * std::cos(60.0 * M_PI / 180.0), 1e-9),
		"voltage-only harmonics add no mean power");

	/* The same 3rd on BOTH lanes carries real power: frac^2 * V * I *
	 * cos(order * displacement). */
	config.harmonics = {{3u, 5.0, 90.0, "all"}};
	expectation = msap1::test::golden_expectation(config);
	require(near(expectation.power[0].active_power_watts,
			1200.0 * std::cos(60.0 * M_PI / 180.0) +
				0.05 * 0.05 * 1200.0 *
					std::cos(3.0 * 60.0 * M_PI / 180.0),
			1e-9),
		"a both-lane harmonic adds frac^2 power at the scaled angle");
}

void harmonic_wire_packing()
{
	auto source = simulator_profile();
	source.simulator.frequency_hz = 60.0;
	source.simulator.harmonics = {{3u, 5.0, 90.0, "voltage"},
				      {5u, 3.0, 0.0, "current"}};
	const auto prepared =
		msap1::prepare_meter_configuration(source, 32000);
	/* Slot 0 word0: order 3, mask 0x70, fraction round(0.05 * 65536). */
	require(prepared.wire.simulator_harmonics[0] ==
			(3u | (0x70u << 8) | (3277u << 16)),
		"harmonic word0 packs order, mask, and Q16 fraction");
	require(prepared.wire.simulator_harmonics[1] == 0x40000000u,
		"harmonic word1 packs the Q0.32 phase");
	require(prepared.wire.simulator_harmonics[2] ==
			(5u | (0x0fu << 8) | (1966u << 16)),
		"second slot packs independently");
	require(prepared.wire.simulator_harmonics[6] == 0u &&
			prepared.wire.simulator_harmonics[7] == 0u,
		"unused slots stay zero");

	/* Rejections: order 1, over-range percent, over-Nyquist order. */
	for (const msap1::SimulatorHarmonicConfig bad :
	     {msap1::SimulatorHarmonicConfig{1u, 5.0, 0.0, "voltage"},
	      msap1::SimulatorHarmonicConfig{3u, 100.0, 0.0, "voltage"},
	      msap1::SimulatorHarmonicConfig{63u, 5.0, 0.0, "voltage"}}) {
		auto rejected = simulator_profile();
		rejected.simulator.frequency_hz = 60.0 * 5.0;
		rejected.simulator.harmonics = {bad};
		bool threw = false;
		try {
			(void)msap1::prepare_meter_configuration(rejected,
								 32000);
		} catch (const std::exception &) {
			threw = true;
		}
		require(threw, "invalid harmonic slots must be rejected");
	}
}

void wire_conversion_round_trip()
{
	auto source = simulator_profile();
	source.simulator.frequency_hz = 60.0;
	source.simulator.preserve_phase = true;
	source.simulator.channels = {
		{0u, 10.0, -60.0, 1.0, 0.1},
		{1u, 0.0, 0.0, 0.0, 0.0},
		{2u, 0.0, 0.0, 0.0, 0.0},
		{3u, 0.0, 0.0, 0.0, 0.0},
		{4u, 0.0, 0.0, 0.0, 0.0},
		{5u, 0.0, 0.0, 0.0, 0.0},
		{6u, 120.0, 0.0, -5.0, 0.5},
	};
	const auto prepared =
		msap1::prepare_meter_configuration(source, 32000);
	const auto &wire = prepared.wire;

	require(wire.simulator_flags == MSAP1_SIMULATOR_FLAG_PRESERVE_PHASE,
		"preserve_phase must set the wire flag");

	/* The engineering->counts conversions must all use the same
	 * per-channel coefficient: counts_per_unit = 1e6 * 2^16 / coeff. */
	for (const std::size_t channel : {std::size_t{0}, std::size_t{6}}) {
		const double counts_per_unit = 1000000.0 * 65536.0 /
			static_cast<double>(wire.scale_micro_units_q16[channel]);
		const auto &spec = source.simulator.channels[channel == 0 ? 0 : 6];
		const double expected_peak =
			spec.rms * std::sqrt(2.0) * counts_per_unit;
		const double expected_dc = spec.dc * counts_per_unit;
		const double expected_noise =
			spec.noise_rms * std::sqrt(3.0) * counts_per_unit;
		require(near(wire.simulator_peak_counts[channel], expected_peak, 1.0),
			"peak counts follow the sqrt(2) RMS conversion");
		require(near(wire.simulator_dc_offset_counts[channel], expected_dc, 1.0),
			"dc counts follow the linear conversion");
		require(near(wire.simulator_noise_level_counts[channel],
				expected_noise, 1.0),
			"noise counts follow the sqrt(3) uniform-RMS conversion");
	}

	/* Round trip: the golden model over the SAME config predicts readings
	 * consistent with what the counts encode, through either conversion. */
	const auto expectation =
		msap1::test::golden_expectation(source.simulator);
	require(near(expectation.channels[0].total_rms,
			std::sqrt(10.0 * 10.0 + 1.0 + 0.01), 1e-9),
		"golden total RMS composes sine, dc, and noise");
	require(expectation.channels[6].mean == -5.0,
		"golden mean is the dc offset");
}

void wire_conversion_rejects_out_of_range()
{
	auto source = simulator_profile();
	source.simulator.channels = {
		{0u, 10.0, 0.0, 1e9, 0.0}, /* absurd DC */
		{1u, 0.0, 0.0, 0.0, 0.0},
		{2u, 0.0, 0.0, 0.0, 0.0},
		{3u, 0.0, 0.0, 0.0, 0.0},
		{4u, 0.0, 0.0, 0.0, 0.0},
		{5u, 0.0, 0.0, 0.0, 0.0},
		{6u, 120.0, 0.0, 0.0, 0.0},
	};
	bool threw = false;
	try {
		(void)msap1::prepare_meter_configuration(source, 32000);
	} catch (const std::exception &) {
		threw = true;
	}
	require(threw, "an out-of-range DC offset must be rejected");
}

void tolerance_scales_with_block_length()
{
	const double loose = msap1::test::golden_rms_tolerance(120.0, 1);
	const double tight = msap1::test::golden_rms_tolerance(120.0, 12);
	require(tight < loose, "longer blocks must tighten the tolerance");
	require(msap1::test::golden_rms_tolerance(0.0) > 0.0,
		"expected-zero channels keep an absolute floor");
}

} // namespace

int main()
{
	golden_expectation_composition();
	true_pf_differs_from_displacement_under_distortion();
	reactive_sign_follows_lag();
	harmonic_expectation_composition();
	harmonic_wire_packing();
	wire_conversion_round_trip();
	wire_conversion_rejects_out_of_range();
	tolerance_scales_with_block_length();
	if (failures != 0) {
		std::fprintf(stderr, "FAILED: %d check(s)\n", failures);
		return EXIT_FAILURE;
	}
	std::printf("PASS: waveform_golden_test\n");
	return EXIT_SUCCESS;
}
