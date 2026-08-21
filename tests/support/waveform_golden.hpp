#ifndef MSAP1_TESTS_WAVEFORM_GOLDEN_HPP
#define MSAP1_TESTS_WAVEFORM_GOLDEN_HPP

/*
 * Double-precision golden model of the PL ADC simulator source.
 *
 * NOT the authoritative implementation: the PL waveform engine
 * (MSAP1_PL/SourceData/HLS_DesignFile/MeterCore/SimWaveEngine) is
 * normative, and its own csim/cosim bench verifies the fixed-point
 * datapath. This model exists so host tests and on-target verification
 * procedures can predict, in engineering units, what the metrology
 * pipeline SHOULD report for a given simulator configuration — the
 * measurement side of the ADC-simulator verification framework
 * (metrology roadmap M0). Per the handover doc it deliberately uses
 * doubles rather than mirroring the fixed-point implementation, so a
 * systematic arithmetic error cannot cancel out between model and DUT.
 *
 * Modelled per enabled channel:
 *   sample(t) = rms*sqrt(2) * sin(2*pi*f*t + phase)
 *             + sum_slots frac * rms*sqrt(2) * sin(order*(2*pi*f*t + phase)
 *                                                  + slot_phase)
 *             + dc + noise
 * with noise uniform white of RMS noise_rms and each harmonic slot applied
 * only to the lanes its channel selector names. Distinct-frequency terms
 * are orthogonal over an integer number of cycles, so:
 *   mean               = dc
 *   fundamental_rms    = rms                            (harmonics rejected)
 *   ac_rms             = sqrt(rms^2*(1 + sum frac^2) + noise_rms^2)
 *   total_rms          = sqrt(ac_rms^2 + dc^2)
 * The PL's own quantization/interpolation error is bounded well below
 * one part in 10^4 of full scale, so tolerances here are dominated by
 * the metrology engine under test, not by the source.
 */

#include "msap1/meter/meter_config.hpp"

#include <array>
#include <cmath>

namespace msap1::test {

struct GoldenChannelExpectation {
	/* Engineering units (volts or amps, per the channel's role). */
	double mean = 0.0;
	double ac_rms = 0.0;
	double total_rms = 0.0;
	/* RMS of the fundamental alone: what the phasor path (SCYC words
	 * 50..63) must report, unchanged by injected harmonics. */
	double fundamental_rms = 0.0;
	bool enabled = false;
};

/* Per-phase active power expectation, watts; import positive (the shared
 * sign conventions). P = Vrms * Irms * cos(phase_v - phase_i) for the
 * fundamental plus the DC product; simulator noise is uncorrelated
 * between channels and contributes no mean power. A harmonic slot that
 * lands on BOTH lanes of a phase ("all") adds frac^2 * Vrms * Irms *
 * cos(order * (phase_v - phase_i)) — the simulator scales each lane's
 * fundamental offset by the order, and the slot phase cancels in the
 * V-I difference. */
struct GoldenPowerExpectation {
	double active_power_watts = 0.0;
	/* S = Vrms_total x Irms_total (TOTAL RMS: harmonics and noise
	 * included -- that is what makes the PF below the TRUE power
	 * factor rather than the displacement factor). */
	double apparent_power_va = 0.0;
	/* True PF = P / S, sign of P; NAN when S is 0 (undefined). */
	double power_factor = 0.0;
	/* Fundamental-only quantities (M9 PHASOR record): the synchronous
	 * correlation rejects DC, harmonics, and noise, so these use the
	 * configured fundamental RMS values alone.
	 *   phi1 = phase_v - phase_i (positive = current lags)
	 *   P1 = V1*I1*cos(phi1)   (no DC term, unlike the true P above)
	 *   Q1 = V1*I1*sin(phi1)   (lagging/inductive positive)
	 *   displacement PF = cos(phi1) = P1/S1 — diverges from the true PF
	 *   under distortion; 0 when S1 = 0 (undefined). */
	double fundamental_active_watts = 0.0;
	double reactive_power_vars = 0.0;
	double displacement_power_factor = 0.0;
	/* Degrees, wrapped to [0, 360) — the PL's published convention. */
	double displacement_angle_degrees = 0.0;
};

/* Symmetrical components of the configured fundamentals (M10 UNBALANCE
 * record): a-operator convention (a = 1 at +120 deg, ABC rotation),
 * X0/X1/X2 as RMS magnitudes, unbalance = |X2|/|X1| and zero ratio =
 * |X0|/|X1| as plain fractions (0 when |X1| = 0 — undefined). */
struct GoldenSequenceExpectation {
	double zero_rms = 0.0;
	double positive_rms = 0.0;
	double negative_rms = 0.0;
	double unbalance = 0.0;
	double zero_ratio = 0.0;
};

/** Expected steady-state readings for one simulator configuration. */
struct GoldenExpectation {
	double frequency_hz = 0.0;
	std::array<GoldenChannelExpectation, 8> channels{};
	/* Phases A/B/C pair voltage lanes 6/5/4 with current lanes 0/1/2. */
	std::array<GoldenPowerExpectation, 3> power{};
	GoldenSequenceExpectation voltage_sequence{};
	GoldenSequenceExpectation current_sequence{};
};

/* Mirrors harmonic_channel_mask in meter_config.cpp (lanes 0..3 current,
 * 4..6 voltage). */
inline unsigned golden_harmonic_mask(const std::string &channels)
{
	if (channels == "voltage")
		return 0x70u;
	if (channels == "current")
		return 0x0fu;
	return 0x7fu; /* "all" */
}

inline GoldenExpectation golden_expectation(const msap1::SimulatorConfig &config)
{
	GoldenExpectation result;
	result.frequency_hz = config.frequency_hz;
	std::array<double, 8> rms{};
	std::array<double, 8> phase_degrees{};
	std::array<double, 8> dc{};
	std::array<double, 8> harmonic_energy{}; /* sum of frac^2 per lane */
	for (const auto &harmonic : config.harmonics) {
		const unsigned mask = golden_harmonic_mask(harmonic.channels);
		const double fraction = harmonic.percent / 100.0;
		for (std::size_t lane = 0; lane < 8; ++lane)
			if (mask & (1u << lane))
				harmonic_energy[lane] += fraction * fraction;
	}
	for (const auto &channel : config.channels) {
		if (channel.channel >= result.channels.size())
			continue;
		auto &expectation = result.channels[channel.channel];
		expectation.enabled = true;
		expectation.mean = channel.dc;
		expectation.fundamental_rms = channel.rms;
		expectation.ac_rms = std::sqrt(
			channel.rms * channel.rms *
				(1.0 + harmonic_energy[channel.channel]) +
			channel.noise_rms * channel.noise_rms);
		expectation.total_rms = std::sqrt(
			expectation.ac_rms * expectation.ac_rms +
			channel.dc * channel.dc);
		rms[channel.channel] = channel.rms;
		phase_degrees[channel.channel] = channel.phase_degrees;
		dc[channel.channel] = channel.dc;
	}
	static constexpr int voltage_lane[3] = {6, 5, 4};
	static constexpr int current_lane[3] = {0, 1, 2};
	for (std::size_t phase = 0; phase < result.power.size(); ++phase) {
		const int v = voltage_lane[phase];
		const int i = current_lane[phase];
		const double angle = (phase_degrees[v] - phase_degrees[i]) *
				     M_PI / 180.0;
		double power = rms[v] * rms[i] * std::cos(angle) + dc[v] * dc[i];
		for (const auto &harmonic : config.harmonics) {
			const unsigned mask =
				golden_harmonic_mask(harmonic.channels);
			if (!(mask & (1u << v)) || !(mask & (1u << i)))
				continue;
			const double fraction = harmonic.percent / 100.0;
			power += fraction * fraction * rms[v] * rms[i] *
				 std::cos(harmonic.order * angle);
		}
		result.power[phase].active_power_watts = power;
		const double s_va = result.channels[v].ac_rms *
				    result.channels[i].ac_rms;
		result.power[phase].apparent_power_va = s_va;
		result.power[phase].power_factor =
			s_va > 0.0 ? power / s_va : 0.0;
		const double s1_va = rms[v] * rms[i];
		result.power[phase].fundamental_active_watts =
			s1_va * std::cos(angle);
		result.power[phase].reactive_power_vars =
			s1_va * std::sin(angle);
		result.power[phase].displacement_power_factor =
			s1_va > 0.0 ? std::cos(angle) : 0.0;
		double disp = phase_degrees[v] - phase_degrees[i];
		while (disp >= 360.0)
			disp -= 360.0;
		while (disp < 0.0)
			disp += 360.0;
		result.power[phase].displacement_angle_degrees = disp;
	}
	const auto sequence_of = [&](const int lanes[3]) {
		GoldenSequenceExpectation out{};
		double re[3], im[3];
		for (int k = 0; k < 3; ++k) {
			const double rad =
				phase_degrees[lanes[k]] * M_PI / 180.0;
			re[k] = rms[lanes[k]] * std::cos(rad);
			im[k] = rms[lanes[k]] * std::sin(rad);
		}
		const double c = -0.5, s = std::sqrt(3.0) / 2.0;
		const auto rotate = [&](double xr, double xi, double sign,
					double &orr, double &oi) {
			orr = xr * c - sign * xi * s;
			oi = sign * xr * s + xi * c;
		};
		double bar, bai, ba2r, ba2i, car, cai, ca2r, ca2i;
		rotate(re[1], im[1], 1.0, bar, bai);    /* a * B */
		rotate(re[1], im[1], -1.0, ba2r, ba2i); /* a^2 * B */
		rotate(re[2], im[2], 1.0, car, cai);
		rotate(re[2], im[2], -1.0, ca2r, ca2i);
		out.zero_rms = std::hypot(re[0] + re[1] + re[2],
					  im[0] + im[1] + im[2]) / 3.0;
		out.positive_rms = std::hypot(re[0] + bar + ca2r,
					      im[0] + bai + ca2i) / 3.0;
		out.negative_rms = std::hypot(re[0] + ba2r + car,
					      im[0] + ba2i + cai) / 3.0;
		if (out.positive_rms > 0.0) {
			out.unbalance = out.negative_rms / out.positive_rms;
			out.zero_ratio = out.zero_rms / out.positive_rms;
		}
		return out;
	};
	result.voltage_sequence = sequence_of(voltage_lane);
	result.current_sequence = sequence_of(current_lane);
	return result;
}

/**
 * Relative tolerance for comparing a measured RMS against the golden
 * expectation on target. Dominated by the metrology chain (24-bit
 * quantization, block windowing against a slightly off-nominal source),
 * not by the simulator: its sine spurs sit below -100 dBc.
 *
 * @param cycles_per_block complete grid cycles in the measurement block
 *        (10/12 for the basic tier); windowing leakage shrinks with it.
 */
inline double golden_rms_tolerance(double expected_rms,
				   unsigned cycles_per_block = 10)
{
	if (expected_rms <= 0.0)
		return 1e-6; /* absolute floor for expected-zero channels */
	const double quantization = 2.0e-4;
	const double windowing = 5.0e-3 / static_cast<double>(cycles_per_block);
	return expected_rms * (quantization + windowing);
}

} // namespace msap1::test

#endif /* MSAP1_TESTS_WAVEFORM_GOLDEN_HPP */
