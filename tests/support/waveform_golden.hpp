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
 *   sample(t) = rms*sqrt(2) * sin(2*pi*f*t + phase) + dc + noise
 * with noise uniform white of RMS noise_rms. Quantities derived over an
 * integer number of cycles:
 *   mean               = dc
 *   ac_rms             = sqrt(rms^2 + noise_rms^2)     (dc removed)
 *   total_rms          = sqrt(rms^2 + noise_rms^2 + dc^2)
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
	bool enabled = false;
};

/* Per-phase active power expectation, watts; import positive (the shared
 * sign conventions). P = Vrms * Irms * cos(phase_v - phase_i) for the
 * fundamental plus the DC product; simulator noise is uncorrelated
 * between channels and contributes no mean power. */
struct GoldenPowerExpectation {
	double active_power_watts = 0.0;
};

/** Expected steady-state readings for one simulator configuration. */
struct GoldenExpectation {
	double frequency_hz = 0.0;
	std::array<GoldenChannelExpectation, 8> channels{};
	/* Phases A/B/C pair voltage lanes 6/5/4 with current lanes 0/1/2. */
	std::array<GoldenPowerExpectation, 3> power{};
};

inline GoldenExpectation golden_expectation(const msap1::SimulatorConfig &config)
{
	GoldenExpectation result;
	result.frequency_hz = config.frequency_hz;
	std::array<double, 8> rms{};
	std::array<double, 8> phase_degrees{};
	std::array<double, 8> dc{};
	for (const auto &channel : config.channels) {
		if (channel.channel >= result.channels.size())
			continue;
		auto &expectation = result.channels[channel.channel];
		expectation.enabled = true;
		expectation.mean = channel.dc;
		expectation.ac_rms = std::sqrt(
			channel.rms * channel.rms +
			channel.noise_rms * channel.noise_rms);
		expectation.total_rms = std::sqrt(
			channel.rms * channel.rms +
			channel.noise_rms * channel.noise_rms +
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
		result.power[phase].active_power_watts =
			rms[v] * rms[i] * std::cos(angle) + dc[v] * dc[i];
	}
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
