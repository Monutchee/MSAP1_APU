#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace msap1::settings {

struct EventWaveformPolicy {
	bool enabled = true;
	std::uint32_t pretrigger_ms = 3000;
	std::uint32_t posttrigger_ms = 3000;
	/* At the 128 kSPS product default, /8 retains 16 kSPS evidence while
	 * reducing event-file storage by 8x. Event classification happens before
	 * this capture policy, on the Urms(1/2) measurement stream. */
	std::uint32_t decimation = 8;

	void validate() const
	{
		if (pretrigger_ms > 120000u || posttrigger_ms > 120000u)
			throw std::runtime_error(
				"event waveform windows must not exceed 120 seconds");
		if (decimation != 1u && decimation != 2u && decimation != 4u &&
		    decimation != 8u && decimation != 16u && decimation != 32u)
			throw std::runtime_error(
				"event waveform decimation must be 1, 2, 4, 8, 16, or 32");
	}
};

struct EventProfileSettings {
	bool enabled = false;
	double threshold_percent = 0.0;
	double hysteresis_percent = 0.0;
	std::uint32_t phase_mask = 0x7u;
	std::string phase_policy = "per_phase";
	EventWaveformPolicy waveform{};

	void validate(std::string_view name) const
	{
		if (!std::isfinite(threshold_percent) || threshold_percent < 0.0 ||
		    threshold_percent > 655.35 ||
		    !std::isfinite(hysteresis_percent) || hysteresis_percent < 0.0 ||
		    hysteresis_percent > 655.35 ||
		    hysteresis_percent >= threshold_percent || phase_mask == 0u ||
		    (phase_mask & ~0x7u) != 0u)
			throw std::runtime_error(std::string(name) +
				" event profile is out of range");
		if (phase_policy != "per_phase" && phase_policy != "polyphase")
			throw std::runtime_error(std::string(name) +
				" phase policy must be per_phase or polyphase");
		waveform.validate();
	}
};

/**
 * Power-quality event policy. The first four profiles are IEC voltage
 * phenomena; the
 * current and unbalance profiles are explicitly product alarms. Transient
 * classification stays unavailable until the analogue path is characterized.
 */
struct PowerQualityEventSettings {
	double reference_current_amperes = 0.0;
	EventProfileSettings voltage_sag{true, 90.0, 2.0};
	EventProfileSettings voltage_swell{true, 110.0, 2.0};
	EventProfileSettings voltage_interruption{true, 10.0, 2.0};
	EventProfileSettings rapid_voltage_change{true, 3.0, 0.5};
	EventProfileSettings voltage_unbalance{
		false, 2.0, 0.2, 0x7u, "polyphase", {false, 3000, 3000, 8}};
	EventProfileSettings current_sag{
		false, 90.0, 2.0, 0x7u, "per_phase", {false, 3000, 3000, 8}};
	EventProfileSettings current_swell{
		false, 110.0, 2.0, 0x7u, "per_phase", {false, 3000, 3000, 8}};
	EventProfileSettings current_unbalance{
		false, 10.0, 1.0, 0x7u, "polyphase", {false, 3000, 3000, 8}};
	EventProfileSettings transient_voltage{
		false, 150.0, 5.0, 0x7u, "per_phase", {false, 3000, 3000, 8}};

	void validate() const
	{
		if (!std::isfinite(reference_current_amperes) ||
		    reference_current_amperes < 0.0 ||
		    reference_current_amperes > 4294.967295)
			throw std::runtime_error(
				"event current reference is out of range");
		voltage_sag.validate("voltage-sag");
		voltage_swell.validate("voltage-swell");
		voltage_interruption.validate("voltage-interruption");
		rapid_voltage_change.validate("rapid-voltage-change");
		voltage_unbalance.validate("voltage-unbalance");
		current_sag.validate("current-sag");
		current_swell.validate("current-swell");
		current_unbalance.validate("current-unbalance");
		transient_voltage.validate("transient-voltage");
		if (transient_voltage.enabled)
			throw std::runtime_error(
				"transient-voltage detection is unavailable until the analogue frontend is characterized");
	}
};

struct FlickerSettings {
	bool enabled = true;
	std::uint32_t phase_mask = 0x7u;
	std::uint32_t lamp_voltage = 120u;
	std::uint32_t live_cadence_ms = 1000u;
	std::uint32_t pst_interval_seconds = 600u;
	std::uint32_t plt_pst_count = 12u;

	void validate() const
	{
		if (phase_mask == 0u || (phase_mask & ~0x7u) != 0u ||
		    (lamp_voltage != 120u && lamp_voltage != 230u) ||
		    live_cadence_ms != 1000u || pst_interval_seconds != 600u ||
		    plt_pst_count != 12u)
			throw std::runtime_error(
				"flicker requires a 120/230 V lamp model, 1 s live cadence, 10 minute Pst, and twelve-value Plt");
	}
};

struct MainsSignallingSettings {
	bool enabled = false;
	double carrier_frequency_hz = 1000.0;
	double bandwidth_hz = 20.0;
	std::uint32_t observation_ms = 200u;
	std::uint32_t phase_mask = 0x7u;
	double threshold_percent = 0.0;

	void validate(std::uint32_t sample_rate_hz) const
	{
		if (!std::isfinite(carrier_frequency_hz) ||
		    !std::isfinite(bandwidth_hz) ||
		    !std::isfinite(threshold_percent) || carrier_frequency_hz <= 0.0 ||
		    carrier_frequency_hz >= 12500.0 || bandwidth_hz < 0.004 ||
		    bandwidth_hz >= carrier_frequency_hz || observation_ms != 200u ||
		    phase_mask == 0u || (phase_mask & ~0x7u) != 0u ||
		    threshold_percent < 0.0 || threshold_percent > 655.35 ||
		    carrier_frequency_hz + bandwidth_hz >= 12500.0)
			throw std::runtime_error(
				"mains-signalling configuration is out of range");
		if (enabled && carrier_frequency_hz + bandwidth_hz >=
			static_cast<double>(sample_rate_hz) / 2.0)
			throw std::runtime_error(
				"mains-signalling band exceeds the selected sample-rate Nyquist limit");
	}
};

} // namespace msap1::settings
