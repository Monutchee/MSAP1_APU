#include "core/cli.hpp"
#include "core/result_output.hpp"

#include "msap1/acquisition/ipc/acquisition_ipc.hpp"
#include "msap1/meter/meter_config.hpp"
#include "msap1/settings/settings.hpp"
#include "msap1/settings/settings_ipc.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <limits>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace msap1::cli {
namespace {

void require_daemon_ok(AcquisitionStatus status)
{
	if (status != AcquisitionStatus::ok)
		throw std::runtime_error("acquisition daemon request failed (status " +
			std::to_string(static_cast<std::uint32_t>(status)) + ")");
}

void require_settings_ok(const msap1::settings::ipc::Response &response)
{
	if (response.status != msap1::settings::ipc::Status::ok)
		throw std::runtime_error(response.message.empty()
			? "settings service rejected the request" : response.message);
}

void update_persistent_settings(
	const Options &options,
	const std::function<void(msap1::settings::ProductSettings &)> &update)
{
	using SettingsCommand = msap1::settings::ipc::Command;
	using SettingsRequest = msap1::settings::ipc::Request;
	msap1::settings::ipc::SettingsClient client;

	SettingsRequest get;
	get.command = SettingsCommand::get_active;
	auto active = client.request(std::move(get), options.timeout_ms);
	require_settings_ok(active);
	auto settings = msap1::settings::SettingsCodec::decode(active.json);
	update(settings);

	SettingsRequest save;
	save.command = SettingsCommand::save_active;
	save.json = msap1::settings::SettingsCodec::encode(settings, false);
	const auto committed = client.request(std::move(save),
		std::max(options.timeout_ms, 30000));
	require_settings_ok(committed);
}

struct CaptureControlResult {
	bool running = false;
};

class CaptureControlTextGenerator final :
	public ResultGenerator<CaptureControlResult> {
public:
	int write(const CaptureControlResult &result,
		  std::ostream &output) const override
	{
		output << "FPGA acquisition "
		       << (result.running ? "running" : "stopped") << '\n';
		return 0;
	}
};

class CaptureControlJsonGenerator final :
	public ResultGenerator<CaptureControlResult> {
public:
	int write(const CaptureControlResult &result,
		  std::ostream &output) const override
	{
		write_json_success(output, result);
		return 0;
	}
};

int run_control(const Options &options, std::ostream &output,
		bool start_capture)
{
	AcquisitionClient client(options.socket_path);
	const auto response = start_capture
		? client.request(StartRequest{}, options.timeout_ms)
		: client.request(StopRequest{}, options.timeout_ms);
	require_daemon_ok(response.status);
	const CaptureControlResult result{response.running};
	return render_result(options, result, output,
			     CaptureControlTextGenerator{},
			     CaptureControlJsonGenerator{});
}

std::uint32_t parse_sample_rate(const std::string &value)
{
	std::size_t end = 0;
	std::uint64_t parsed = 0;
	try {
		parsed = std::stoull(value, &end, 0);
	} catch (const std::exception &) {
		throw std::invalid_argument("--sps requires a supported sample rate");
	}
	if (end != value.size() ||
	    parsed > static_cast<std::uint64_t>(
			     std::numeric_limits<std::uint32_t>::max()) ||
	    !supported_adc_sample_rate(static_cast<std::uint32_t>(parsed)))
		throw std::invalid_argument(
			"--sps must be one of 1000, 2000, 4000, 8000, 16000, "
			"32000, 64000, or 128000");
	return static_cast<std::uint32_t>(parsed);
}

double src_derived_rate(std::uint32_t dclk_frequency_hz,
			std::uint8_t general_user_config_1,
			std::uint8_t dout_format,
			std::uint8_t src_n_msb, std::uint8_t src_n_lsb,
			std::uint8_t src_if_msb, std::uint8_t src_if_lsb)
{
	if (dclk_frequency_hz == 0u)
		return 0.0;
	const auto src_n = static_cast<std::uint16_t>(
		(static_cast<std::uint16_t>(src_n_msb & 0x0fu) << 8) |
		src_n_lsb);
	const auto src_if = static_cast<std::uint16_t>(
		(static_cast<std::uint16_t>(src_if_msb) << 8) | src_if_lsb);
	const auto decimation = static_cast<double>(src_n) +
		static_cast<double>(src_if) / 65536.0;
	if (decimation <= 0.0)
		return 0.0;
	const auto dclk_divisor =
		static_cast<double>(1u << ((dout_format >> 1) & 0x07u));
	const auto modulator_divisor =
		(general_user_config_1 & 0x40u) != 0u ? 4.0 : 8.0;
	return static_cast<double>(dclk_frequency_hz) * dclk_divisor /
		(modulator_divisor * decimation);
}

double src_derived_rate(const msap1_adc_health_payload &health)
{
	return src_derived_rate(
		health.dclk_frequency_hz, health.general_user_config_1,
		health.dout_format, health.src_n_msb, health.src_n_lsb,
		health.src_if_msb, health.src_if_lsb);
}

bool rate_measurements_agree(std::uint32_t first, std::uint32_t second)
{
	if (first == 0u || second == 0u)
		return false;
	const auto difference = first > second ? first - second : second - first;
	return difference <= std::max(2u, second / 1000u);
}

struct AdcRateResult {
	std::uint32_t requested_hz = 0;
	std::uint32_t dclk_hz = 0;
	double src_derived_hz = 0.0;
	std::uint32_t measured_drdy_hz = 0;
	bool measurement_available = false;
	bool matches = false;
};

class AdcRateTextGenerator final : public ResultGenerator<AdcRateResult> {
public:
	int write(const AdcRateResult &result,
		  std::ostream &output) const override
	{
		output << "ADC sample rate\n"
		       << "  Requested:            " << result.requested_hz
		       << " frame/s\n"
		       << "  ADC DCLK:             ";
		if (result.dclk_hz != 0u)
			output << result.dclk_hz << " Hz\n";
		else
			output << "unavailable\n";
		output << "  SRC-derived rate:     ";
		if (result.src_derived_hz > 0.0)
			output << std::fixed << std::setprecision(3)
			       << result.src_derived_hz << " frame/s\n";
		else
			output << "unavailable\n";
		output << "  Measured ADC DRDY:    ";
		if (result.measurement_available)
			output << result.measured_drdy_hz << " frame/s\n";
		else
			output << "unavailable\n";
		output << "  Result:               "
		       << (!result.measurement_available
				   ? "measurement unavailable"
				   : result.matches ? "match" : "MISMATCH")
		       << '\n';
		return 0;
	}
};

class AdcRateJsonGenerator final : public ResultGenerator<AdcRateResult> {
public:
	int write(const AdcRateResult &result,
		  std::ostream &output) const override
	{
		write_json_success(output, result);
		return 0;
	}
};

int run_rate(const Options &options, std::ostream &output)
{
	AcquisitionClient client(options.socket_path);
	InfoResponse response;
	if (options.sample_rate_hz) {
		SampleRateSetRequest set_rate;
		set_rate.sample_rate_hz = *options.sample_rate_hz;
		response = client.request(set_rate, options.timeout_ms);
		require_daemon_ok(response.status);
		/*
		 * The PL publishes DRDY over independent one-second windows. Poll
		 * across window boundaries until two consecutive measurements agree;
		 * a single fixed delay can still return the pre-transition window.
		 */
		std::uint32_t previous_measurement = 0u;
		for (unsigned int attempt = 0; attempt < 5; ++attempt) {
			std::this_thread::sleep_for(
				std::chrono::milliseconds(1100));
			response = client.request(HealthRefreshRequest{},
				options.timeout_ms);
			require_daemon_ok(response.status);
			const auto current =
				response.rpu_health.value().drdy_frequency_hz;
			if (rate_measurements_agree(previous_measurement, current))
				break;
			previous_measurement = current;
		}
	} else {
		// Read the daemon cache for the diagnostic getter. A fresh SPI audit
		// is maintenance work and must not be triggered by restricted users.
		response = client.request(HealthRequest{}, options.timeout_ms);
	}
	require_daemon_ok(response.status);

	const auto health = response.rpu_health.value();
	const auto derived = src_derived_rate(health);
	const auto measured = health.drdy_frequency_hz;
	const auto requested = response.sample_rate_hz;
	const bool measurement_available = measured != 0u;
	const bool matches = measurement_available &&
		std::abs(static_cast<double>(measured) -
			 static_cast<double>(requested)) <=
			std::max(2.0, static_cast<double>(requested) * 0.01);

	const AdcRateResult result{
		.requested_hz = requested,
		.dclk_hz = health.dclk_frequency_hz,
		.src_derived_hz = derived,
		.measured_drdy_hz = measured,
		.measurement_available = measurement_available,
		.matches = matches,
	};
	return render_result(options, result, output, AdcRateTextGenerator{},
			     AdcRateJsonGenerator{});
}

double parse_finite_double(const std::string &value, const char *option)
{
	std::size_t end = 0;
	double result = 0.0;
	try {
		result = std::stod(value, &end);
	} catch (const std::exception &) {
		throw std::invalid_argument(std::string(option) +
			" requires a numeric value");
	}
	if (end != value.size() || !std::isfinite(result))
		throw std::invalid_argument(std::string(option) +
			" requires a finite numeric value");
	return result;
}

std::uint32_t adc_source_value(const std::string &source)
{
	if (source == "physical")
		return MSAP1_ADC_SOURCE_PHYSICAL;
	if (source == "simulator")
		return MSAP1_ADC_SOURCE_SIMULATOR;
	throw std::invalid_argument("--set must be physical or simulator");
}

const char *adc_source_text(std::uint32_t source)
{
	return source == MSAP1_ADC_SOURCE_SIMULATOR ? "simulator" : "physical";
}

struct AdcSourceResult {
	std::string source;
	bool running = false;
	std::uint32_t generation = 0;
};

class AdcSourceTextGenerator final : public ResultGenerator<AdcSourceResult> {
public:
	int write(const AdcSourceResult &result,
		  std::ostream &output) const override
	{
		output << "ADC source\n"
		       << "  Active source:        " << result.source << '\n'
		       << "  Capture running:      "
		       << (result.running ? "yes" : "no") << '\n'
		       << "  Configuration gen:    0x" << std::hex
		       << result.generation << std::dec << '\n';
		return 0;
	}
};

class AdcSourceJsonGenerator final : public ResultGenerator<AdcSourceResult> {
public:
	int write(const AdcSourceResult &result,
		  std::ostream &output) const override
	{
		write_json_success(output, result);
		return 0;
	}
};

int run_source(const Options &options, std::ostream &output)
{
	AcquisitionClient client(options.socket_path);
	if (options.adc_source) {
		(void)adc_source_value(*options.adc_source);
		update_persistent_settings(options,
			[&](auto &settings) {
				settings.adc.source = *options.adc_source;
			});
	}
	const auto response = client.request(AdcSourceGetRequest{},
		options.timeout_ms);
	require_daemon_ok(response.status);
	const AdcSourceResult result{
		adc_source_text(response.adc_source), response.running,
		response.configuration_generation};
	return render_result(options, result, output, AdcSourceTextGenerator{},
			     AdcSourceJsonGenerator{});
}

struct AdcSimulatorResult {
	double frequency_hz = 0.0;
	bool preserve_phase = false;
	std::array<double, 7> rms{};
	std::array<double, 7> phase_degrees{};
	std::array<double, 7> dc{};
	std::array<double, 7> noise_rms{};
	std::vector<msap1::SimulatorIpcHarmonic> harmonics{};
	bool active = false;
	std::uint32_t generation = 0;
};

class AdcSimulatorTextGenerator final :
	public ResultGenerator<AdcSimulatorResult> {
public:
	int write(const AdcSimulatorResult &result,
		  std::ostream &output) const override
	{
		static constexpr std::array<const char *, 7> names{
			"Ia", "Ib", "Ic", "In", "Vc", "Vb", "Va"};
		output << "ADC simulator\n"
		       << "  Active:               "
		       << (result.active ? "yes" : "no") << '\n'
		       << "  Frequency:            " << std::fixed
		       << std::setprecision(3) << result.frequency_hz << " Hz\n"
		       << "  Configuration gen:    0x" << std::hex
		       << result.generation << std::dec << '\n'
		       << "  Preserve phase:       "
		       << (result.preserve_phase ? "yes" : "no") << '\n';
		for (std::size_t channel = 0; channel < names.size(); ++channel)
			output << "  CH" << channel << ' ' << names[channel]
			       << ": " << result.rms[channel] << " RMS, "
			       << result.phase_degrees[channel] << " deg, "
			       << result.dc[channel] << " DC, "
			       << result.noise_rms[channel] << " noise RMS\n";
		for (const auto &harmonic : result.harmonics)
			output << "  Harmonic " << harmonic.order << ": "
			       << harmonic.percent << " %, "
			       << harmonic.phase_degrees << " deg, "
			       << harmonic.channels << " lanes\n";
		return 0;
	}
};

class AdcSimulatorJsonGenerator final :
	public ResultGenerator<AdcSimulatorResult> {
public:
	int write(const AdcSimulatorResult &result,
		  std::ostream &output) const override
	{
		write_json_success(output, result);
		return 0;
	}
};

/* "none" clears; otherwise "ORDER:PERCENT[:PHASE[:CHANNELS]]" slots
 * separated by commas, e.g. "3:5,5:3:0:current". Range checking stays in
 * the settings authority (prepare_meter_configuration). */
std::vector<msap1::SimulatorHarmonicConfig> parse_harmonics(
	const std::string &specification)
{
	std::vector<msap1::SimulatorHarmonicConfig> result;
	if (specification == "none")
		return result;
	std::istringstream slots(specification);
	std::string slot;
	while (std::getline(slots, slot, ',')) {
		std::istringstream fields(slot);
		std::string field;
		std::array<std::string, 4> parts{};
		std::size_t count = 0;
		while (std::getline(fields, field, ':')) {
			if (count >= parts.size())
				throw std::invalid_argument(
					"--harmonics slot has too many fields");
			parts[count++] = field;
		}
		if (count < 2)
			throw std::invalid_argument(
				"--harmonics slot needs at least ORDER:PERCENT");
		msap1::SimulatorHarmonicConfig harmonic;
		harmonic.order = static_cast<std::uint32_t>(
			std::stoul(parts[0]));
		harmonic.percent = std::stod(parts[1]);
		if (count > 2 && !parts[2].empty())
			harmonic.phase_degrees = std::stod(parts[2]);
		if (count > 3 && !parts[3].empty())
			harmonic.channels = parts[3];
		result.push_back(harmonic);
	}
	return result;
}

/*
 * Channel selection for an event burst. The coarse groups match the
 * harmonic spec's vocabulary; individual lanes are accepted by name so a
 * single-phase sag -- the case the polyphase begin/end rule exists for --
 * is expressible.
 */
std::uint32_t event_channel_mask(const std::string &channels)
{
	static constexpr std::array<const char *, 7> lane_names{
		"ia", "ib", "ic", "in", "vc", "vb", "va"};
	if (channels == "voltage")
		return 0x70u;
	if (channels == "current")
		return 0x0fu;
	if (channels == "all")
		return 0x7fu;
	std::uint32_t mask = 0;
	std::size_t start = 0;
	while (start <= channels.size()) {
		const auto comma = channels.find(',', start);
		const auto name = channels.substr(start,
			comma == std::string::npos ? std::string::npos
						   : comma - start);
		if (name.empty())
			throw std::invalid_argument(
				"--channels has an empty channel name");
		const auto found = std::find_if(lane_names.begin(),
			lane_names.end(), [&](const char *lane) {
				return name == lane;
			});
		if (found == lane_names.end())
			throw std::invalid_argument(
				"--channels accepts voltage, current, all, or "
				"lane names (ia, ib, ic, in, vc, vb, va)");
		mask |= 1u << std::distance(lane_names.begin(), found);
		if (comma == std::string::npos)
			break;
		start = comma + 1;
	}
	if (mask == 0u)
		throw std::invalid_argument("--channels selected no channels");
	return mask;
}

std::uint32_t parse_half_cycles(const std::string &value, const char *option)
{
	std::size_t end = 0;
	unsigned long long parsed = 0;
	try {
		parsed = std::stoull(value, &end, 10);
	} catch (const std::exception &) {
		end = 0;
	}
	if (end != value.size() || parsed > 65535ull)
		throw std::invalid_argument(
			std::string(option) + " requires 0..65535 half cycles");
	return static_cast<std::uint32_t>(parsed);
}

std::uint32_t event_action_value(const std::string &action)
{
	if (action == "arm")
		return MSAP1_SIMULATOR_EVENT_ARM;
	if (action == "cancel")
		return MSAP1_SIMULATOR_EVENT_CANCEL;
	if (action == "clear")
		return MSAP1_SIMULATOR_EVENT_CLEAR_COUNT;
	if (action == "query")
		return MSAP1_SIMULATOR_EVENT_QUERY;
	throw std::invalid_argument(
		"--action must be arm, cancel, clear, or query");
}

struct AdcSimulatorEventResult {
	std::string action = "query";
	bool armed = false;
	bool running = false;
	bool holding = false;
	std::uint32_t completed = 0;
	std::uint32_t remaining_half_cycles = 0;
	std::uint32_t until_repeat_half_cycles = 0;
	std::uint32_t channel_mask = 0;
	double scale_percent = 100.0;
	std::uint32_t duration_half_cycles = 0;
	std::uint32_t period_half_cycles = 0;
	bool repeat = false;
	bool simulator_active = false;
};

class AdcSimulatorEventTextGenerator final :
	public ResultGenerator<AdcSimulatorEventResult> {
public:
	int write(const AdcSimulatorEventResult &result,
		  std::ostream &output) const override
	{
		output << "ADC simulator event sequencer\n"
		       << "  Action:      " << result.action << '\n'
		       << "  State:       "
		       << (result.running ? "running"
			   : result.holding ? "holding"
			   : result.armed   ? "armed"
					    : "idle")
		       << '\n'
		       << "  Completed:   " << result.completed << " burst(s)\n";
		if (result.running)
			output << "  Remaining:   "
			       << result.remaining_half_cycles
			       << " half cycle(s)\n";
		if (result.repeat)
			output << "  Next burst:  "
			       << result.until_repeat_half_cycles
			       << " half cycle(s)\n";
		output << "  Committed:   channels 0x" << std::hex
		       << result.channel_mask << std::dec << ", "
		       << result.scale_percent << " %, "
		       << result.duration_half_cycles << " half cycle(s)";
		if (result.repeat)
			output << ", repeating every "
			       << result.period_half_cycles << " half cycle(s)";
		output << '\n';
		if (!result.simulator_active)
			output << "  NOTE: the simulator is not the selected "
				  "ADC source\n";
		return 0;
	}
};

class AdcSimulatorEventJsonGenerator final :
	public ResultGenerator<AdcSimulatorEventResult> {
public:
	int write(const AdcSimulatorEventResult &result,
		  std::ostream &output) const override
	{
		output << glz::write_json(result).value_or("{}");
		return 0;
	}
};

int run_simulator_event(const Options &options, std::ostream &output)
{
	msap1::SimulatorEventRequest request{};
	const std::string action = options.event_action.value_or("query");
	request.action = event_action_value(action);
	if (request.action == MSAP1_SIMULATOR_EVENT_ARM) {
		if (!options.event_channels || !options.event_scale_percent ||
		    !options.event_duration_half_cycles)
			throw std::invalid_argument(
				"--action arm requires --channels, --scale-percent, "
				"and --duration");
		request.channel_mask = event_channel_mask(*options.event_channels);
		request.scale_percent = *options.event_scale_percent;
		request.duration_half_cycles = *options.event_duration_half_cycles;
		request.period_half_cycles =
			options.event_period_half_cycles.value_or(0u);
		request.repeat = options.event_repeat.value_or(false);
	}

	AcquisitionClient client(options.socket_path);
	const auto response = client.request(request, options.timeout_ms);
	require_daemon_ok(response.status);

	AdcSimulatorEventResult result{};
	result.action = action;
	result.armed = (response.sequencer_status & 0x1u) != 0u;
	result.running = (response.sequencer_status & 0x2u) != 0u;
	result.holding = (response.sequencer_status & 0x4u) != 0u;
	result.completed = response.sequencer_status >> 16;
	result.remaining_half_cycles = response.remaining & 0xffffu;
	result.until_repeat_half_cycles = response.remaining >> 16;
	result.channel_mask = response.active_control & 0xffu;
	result.repeat = (response.active_control & (1u << 8)) != 0u;
	result.scale_percent =
		static_cast<double>(response.active_scale) * 100.0 / 65536.0;
	result.duration_half_cycles = response.active_timing & 0xffffu;
	result.period_half_cycles = response.active_timing >> 16;
	result.simulator_active =
		response.adc_source == MSAP1_ADC_SOURCE_SIMULATOR;
	return render_result(options, result, output,
			     AdcSimulatorEventTextGenerator{},
			     AdcSimulatorEventJsonGenerator{});
}

int run_simulator(const Options &options, std::ostream &output)
{
	AcquisitionClient client(options.socket_path);
	auto response = client.request(SimulatorGetRequest{},
		options.timeout_ms);
	require_daemon_ok(response.status);
	const auto any_set = [](const auto &values) {
		return std::any_of(values.begin(), values.end(),
			[](const auto &value) { return value.has_value(); });
	};
	const bool configure = options.simulator_frequency_hz.has_value() ||
		options.simulator_preserve_phase.has_value() ||
		any_set(options.simulator_rms) ||
		any_set(options.simulator_phase_degrees) ||
		any_set(options.simulator_dc) ||
		any_set(options.simulator_noise_rms) ||
		options.simulator_harmonics.has_value();
	if (configure) {
		update_persistent_settings(options,
			[&](auto &settings) {
				if (options.simulator_frequency_hz)
					settings.adc.simulator.frequency_hz =
						*options.simulator_frequency_hz;
				if (options.simulator_preserve_phase)
					settings.adc.simulator.preserve_phase =
						*options.simulator_preserve_phase;
				if (options.simulator_harmonics)
					settings.adc.simulator.harmonics =
						parse_harmonics(*options
							.simulator_harmonics);
				for (std::size_t channel = 0; channel < 7u; ++channel) {
					auto &target = settings.adc.simulator.channels[channel];
					if (options.simulator_rms[channel])
						target.rms = *options.simulator_rms[channel];
					if (options.simulator_phase_degrees[channel])
						target.phase_degrees =
							*options.simulator_phase_degrees[channel];
					if (options.simulator_dc[channel])
						target.dc = *options.simulator_dc[channel];
					if (options.simulator_noise_rms[channel])
						target.noise_rms =
							*options.simulator_noise_rms[channel];
				}
			});
		response = client.request(SimulatorGetRequest{},
			options.timeout_ms);
		require_daemon_ok(response.status);
	}
	AdcSimulatorResult result{};
	result.frequency_hz =
		static_cast<double>(response.simulator.frequency_millihz) / 1000.0;
	result.preserve_phase = response.simulator.preserve_phase != 0u;
	for (std::size_t channel = 0; channel < 7u; ++channel) {
		result.rms[channel] = response.simulator.channels[channel].rms;
		result.phase_degrees[channel] =
			response.simulator.channels[channel].phase_degrees;
		result.dc[channel] = response.simulator.channels[channel].dc;
		result.noise_rms[channel] =
			response.simulator.channels[channel].noise_rms;
	}
	result.harmonics = response.simulator.harmonics;
	result.active = response.adc_source == MSAP1_ADC_SOURCE_SIMULATOR;
	result.generation = response.configuration_generation;
	return render_result(options, result, output, AdcSimulatorTextGenerator{},
			     AdcSimulatorJsonGenerator{});
}

std::uint32_t parse_diagnostic_flow(const std::string &value)
{
	std::size_t end = 0;
	std::uint64_t parsed = 0;
	try {
		parsed = std::stoull(value, &end, 0);
	} catch (const std::exception &) {
		throw std::invalid_argument("--flow requires diagnostic flow 1");
	}
	if (end != value.size() || parsed != 1u)
		throw std::invalid_argument("--flow currently supports only flow 1");
	return 1u;
}

const char *diagnostic_stage_name(std::uint32_t stage)
{
	switch (stage) {
	case MSAP1_ADC_DIAGNOSTIC_STAGE_NONE: return "none";
	case MSAP1_ADC_DIAGNOSTIC_STAGE_PREFLIGHT: return "preflight";
	case MSAP1_ADC_DIAGNOSTIC_STAGE_BEFORE: return "before snapshot";
	case MSAP1_ADC_DIAGNOSTIC_STAGE_RESET_ASSERT: return "RESET_N asserted";
	case MSAP1_ADC_DIAGNOSTIC_STAGE_RESET_RELEASE: return "RESET_N release";
	case MSAP1_ADC_DIAGNOSTIC_STAGE_RESET_DEFAULTS:
		return "reset-default snapshot";
	case MSAP1_ADC_DIAGNOSTIC_STAGE_RECONFIGURE:
		return "conservative SRC reload";
	case MSAP1_ADC_DIAGNOSTIC_STAGE_AFTER: return "after snapshot";
	default: return "unknown";
	}
}

const char *diagnostic_error_name(std::uint32_t error)
{
	switch (error) {
	case MSAP1_ADC_DIAGNOSTIC_ERROR_NONE: return "none";
	case MSAP1_ADC_DIAGNOSTIC_ERROR_NOT_INITIALIZED:
		return "ADC not initialized";
	case MSAP1_ADC_DIAGNOSTIC_ERROR_CAPTURE_ACTIVE:
		return "capture was not stopped";
	case MSAP1_ADC_DIAGNOSTIC_ERROR_SPI: return "SPI communication";
	case MSAP1_ADC_DIAGNOSTIC_ERROR_ADC_NOT_READY:
		return "INIT_COMPLETE timeout";
	case MSAP1_ADC_DIAGNOSTIC_ERROR_REGISTER_MISMATCH:
		return "register readback mismatch";
	default: return "internal error";
	}
}

void print_hex_byte(std::ostream &output, std::uint8_t value)
{
	output << "0x" << std::hex << std::setw(2) << std::setfill('0')
	       << static_cast<unsigned int>(value) << std::dec
	       << std::setfill(' ');
}

void print_diagnostic_snapshot(
	std::ostream &output, const char *name,
	const msap1_adc_diagnostic_snapshot &snapshot)
{
	output << "\n" << name << "\n"
	       << "  PL capture flags:     0x" << std::hex
	       << snapshot.capture_flags << std::dec << "\n"
	       << "  PL frames/packets:    " << snapshot.frame_count << " / "
	       << snapshot.packet_count << "\n"
	       << "  ADC DCLK:             ";
	if (snapshot.dclk_frequency_hz != 0u)
		output << snapshot.dclk_frequency_hz << " Hz\n";
	else
		output << "unavailable/0\n";
	output << "  ADC DRDY:             ";
	if (snapshot.drdy_frequency_hz != 0u)
		output << snapshot.drdy_frequency_hz << " frame/s\n";
	else
		output << "unavailable/0\n";

	if ((snapshot.snapshot_flags &
	     MSAP1_ADC_DIAGNOSTIC_SNAPSHOT_SPI_VALID) == 0u) {
		output << "  SPI registers:        unavailable while RESET_N is low\n";
		return;
	}

	const auto src_n = static_cast<std::uint16_t>(
		(static_cast<std::uint16_t>(snapshot.src_n_msb & 0x0fu) << 8) |
		snapshot.src_n_lsb);
	const auto src_if = static_cast<std::uint16_t>(
		(static_cast<std::uint16_t>(snapshot.src_if_msb) << 8) |
		snapshot.src_if_lsb);
	const auto derived = src_derived_rate(
		snapshot.dclk_frequency_hz,
		snapshot.general_user_config_1, snapshot.dout_format,
		snapshot.src_n_msb, snapshot.src_n_lsb,
		snapshot.src_if_msb, snapshot.src_if_lsb);

	output << "  STATUS 1/2/3:        ";
	print_hex_byte(output, snapshot.status_1);
	output << " / ";
	print_hex_byte(output, snapshot.status_2);
	output << " / ";
	print_hex_byte(output, snapshot.status_3);
	output << "\n  CONFIG 1/2/3:        ";
	print_hex_byte(output, snapshot.general_user_config_1);
	output << " / ";
	print_hex_byte(output, snapshot.general_user_config_2);
	output << " / ";
	print_hex_byte(output, snapshot.general_user_config_3);
	output << "\n  DOUT_FORMAT:          ";
	print_hex_byte(output, snapshot.dout_format);
	output << "\n  CHANNEL_DISABLE:      ";
	print_hex_byte(output, snapshot.channel_disable);
	output << "\n  BUFFER_CONFIG 1/2:   ";
	print_hex_byte(output, snapshot.buffer_config_1);
	output << " / ";
	print_hex_byte(output, snapshot.buffer_config_2);
	output << "\n  SRC N / IF:           " << src_n << " / " << src_if
	       << "\n  SRC_UPDATE:           ";
	print_hex_byte(output, snapshot.src_update);
	output << "\n  SRC-derived rate:     ";
	if (derived > 0.0)
		output << std::fixed << std::setprecision(3) << derived
		       << " frame/s\n";
	else
		output << "unavailable\n";
}

const char *yes_no(bool value)
{
	return value ? "yes" : "no";
}

int run_test_flow(const Options &options, std::ostream &output)
{
	if (!options.diagnostic_flow)
		throw std::invalid_argument("mnc adc testflw requires --flow 1");

	AcquisitionClient client(options.socket_path);
	const auto timeout = std::max(options.timeout_ms, 20000);
	DiagnosticRunRequest run;
	run.flow = *options.diagnostic_flow;
	const auto response = client.request(run, timeout);
	require_daemon_ok(response.status);
	const auto diagnostic = response.diagnostic.value();
	const auto flags = diagnostic.diagnostic_flags;

	output << "AD7771 diagnostic test flow " << diagnostic.flow << "\n"
	       << "  Reset method:         PL-driven ADC RESET_N pulse\n"
	       << "  FPGA/Linux reset:     no\n"
	       << "  ADC power cycle:      no\n"
	       << "  RESET_N hold:         " << diagnostic.reset_hold_ms << " ms\n"
	       << "  Requested sample rate:" << std::setw(9)
	       << diagnostic.requested_sample_rate_hz << " frame/s\n"
	       << "  Flow error:           "
	       << diagnostic_error_name(diagnostic.diagnostic_error) << "\n"
	       << "  Failure stage:        "
	       << diagnostic_stage_name(diagnostic.failure_stage) << "\n"
	       << "  Acquisition restored: "
	       << yes_no(response.running) << "\n";

	print_diagnostic_snapshot(output, "Before reset", diagnostic.before);
	print_diagnostic_snapshot(
		output, "While RESET_N asserted", diagnostic.reset_asserted);
	print_diagnostic_snapshot(
		output, "Reset defaults (before configuration)",
		diagnostic.reset_defaults);
	print_diagnostic_snapshot(
		output, "After conservative SRC reload", diagnostic.after);

	output << "\nFlow checks\n"
	       << "  RESET_N commanded:    "
	       << yes_no((flags & MSAP1_ADC_DIAGNOSTIC_RESET_ASSERTED) != 0u)
	       << "\n  DRDY stopped in reset:"
	       << std::setw(9)
	       << yes_no((flags &
			  MSAP1_ADC_DIAGNOSTIC_RESET_DRDY_STOPPED) != 0u)
	       << "\n  Reset defaults read:  "
	       << yes_no((flags &
			  MSAP1_ADC_DIAGNOSTIC_RESET_DEFAULTS_READ) != 0u)
	       << "\n  SRC_UPDATE read high: "
	       << yes_no((flags &
			  MSAP1_ADC_DIAGNOSTIC_SRC_UPDATE_HIGH_READ) != 0u)
	       << " (";
	print_hex_byte(output, diagnostic.src_update_high_readback);
	output << ")\n  SRC_UPDATE read low:  "
	       << yes_no((flags &
			  MSAP1_ADC_DIAGNOSTIC_SRC_UPDATE_LOW_READ) != 0u)
	       << " (";
	print_hex_byte(output, diagnostic.src_update_low_readback);
	output << ")\n  SRC holding match:    "
	       << yes_no((flags &
			  MSAP1_ADC_DIAGNOSTIC_SRC_HOLDING_MATCH) != 0u)
	       << "\n  Final config match:   "
	       << yes_no((flags &
			  MSAP1_ADC_DIAGNOSTIC_FINAL_CONFIG_MATCH) != 0u)
	       << "\n  Final DRDY match:     "
	       << yes_no((flags &
			  MSAP1_ADC_DIAGNOSTIC_FINAL_DRDY_MATCH) != 0u)
	       << "\n  Restored live DRDY:   ";
	if (response.live_drdy_frequency_hz != 0u)
		output << response.live_drdy_frequency_hz << " frame/s\n";
	else
		output << "unavailable\n";

	if (diagnostic.diagnostic_error !=
	    MSAP1_ADC_DIAGNOSTIC_ERROR_NONE)
		output << "\nConclusion: Flow did not complete; use the failure stage "
			  "and snapshots above.\n";
	else if ((flags & MSAP1_ADC_DIAGNOSTIC_FINAL_DRDY_MATCH) == 0u)
		output << "\nConclusion: RESET_N and the conservative SRC load "
			  "completed, but physical DRDY still does not match the "
			  "requested rate.\n";
	else
		output << "\nConclusion: Physical DRDY matches the requested rate "
			  "after reset and SRC reload.\n";
	return 0;
}

} // namespace

void register_adc_commands(Application &application)
{
	Command adc("adc", "Control ADC capture and FPGA acquisition");
	Command source(
		"source", "Inspect or select the physical ADC/simulator source",
		run_source,
		{
			.access = AccessLevel::diagnostic,
			.side_effect = SideEffect::none,
			.supports_text = true,
			.supports_json = true,
			.variants = {
				{"--set physical|simulator",
				 AccessLevel::operator_control,
				 "Transactionally select the ADC source"},
			},
		});
	source.set_access_resolver([](const Options &options) {
		return options.adc_source ? AccessLevel::operator_control
					  : AccessLevel::diagnostic;
	});
	source.add_option({
		"set", "SOURCE", "Select physical or simulator input",
		CompletionKind::none,
		[](Options &options, const std::string &value) {
			(void)adc_source_value(value);
			options.adc_source = value;
		},
	});
	adc.add_subcommand(std::move(source));

	Command simulator("simulator", "Inspect or configure the PL ADC simulator");
	simulator.add_subcommand(Command(
		"show", "Show the persisted ADC simulator configuration",
		run_simulator,
		{
			.access = AccessLevel::diagnostic,
			.side_effect = SideEffect::none,
			.supports_text = true,
			.supports_json = true,
			.variants = {},
		}));
	Command simulator_configure(
		"configure", "Configure simulator RMS amplitudes and phases",
		run_simulator,
		{
			.access = AccessLevel::operator_control,
			.side_effect = SideEffect::control,
			.supports_text = true,
			.supports_json = true,
			.variants = {},
		});
	simulator_configure.add_option({
		"frequency-hz", "HZ", "Fundamental frequency",
		CompletionKind::none,
		[](Options &options, const std::string &value) {
			const auto parsed = parse_finite_double(value, "--frequency-hz");
			if (parsed <= 0.0)
				throw std::invalid_argument(
					"--frequency-hz must be positive");
			options.simulator_frequency_hz = parsed;
		},
	});
	static constexpr std::array<const char *, 7> channel_options{
		"ia", "ib", "ic", "in", "vc", "vb", "va"};
	for (std::size_t channel = 0; channel < channel_options.size(); ++channel) {
		const std::string rms_option =
			std::string(channel_options[channel]) + "-rms";
		simulator_configure.add_option({
			rms_option, "VALUE", "Channel engineering RMS amplitude",
			CompletionKind::none,
			[channel, rms_option](Options &options,
					      const std::string &value) {
				const auto parsed = parse_finite_double(
					value, ("--" + rms_option).c_str());
				if (parsed < 0.0)
					throw std::invalid_argument(
						"simulator RMS must not be negative");
				options.simulator_rms[channel] = parsed;
			},
		});
		const std::string phase_option =
			std::string(channel_options[channel]) + "-phase-degrees";
		simulator_configure.add_option({
			phase_option, "DEGREES", "Channel phase offset",
			CompletionKind::none,
			[channel, phase_option](Options &options,
						const std::string &value) {
				options.simulator_phase_degrees[channel] =
					parse_finite_double(
						value,
						("--" + phase_option).c_str());
			},
		});
		const std::string dc_option =
			std::string(channel_options[channel]) + "-dc";
		simulator_configure.add_option({
			dc_option, "VALUE", "Channel DC offset (engineering units)",
			CompletionKind::none,
			[channel, dc_option](Options &options,
					     const std::string &value) {
				options.simulator_dc[channel] =
					parse_finite_double(
						value, ("--" + dc_option).c_str());
			},
		});
		const std::string noise_option =
			std::string(channel_options[channel]) + "-noise-rms";
		simulator_configure.add_option({
			noise_option, "VALUE",
			"Channel fluctuation RMS (engineering units)",
			CompletionKind::none,
			[channel, noise_option](Options &options,
						const std::string &value) {
				const auto parsed = parse_finite_double(
					value, ("--" + noise_option).c_str());
				if (parsed < 0.0)
					throw std::invalid_argument(
						"simulator noise RMS must not be negative");
				options.simulator_noise_rms[channel] = parsed;
			},
		});
	}
	simulator_configure.add_option({
		"harmonics", "SPEC",
		"Harmonic slots: none, or ORDER:PERCENT[:PHASE[:CHANNELS]] "
		"comma-separated (channels: voltage, current, all)",
		CompletionKind::none,
		[](Options &options, const std::string &value) {
			options.simulator_harmonics = value;
		},
		true,
	});
	simulator_configure.add_option({
		"preserve-phase", "BOOL",
		"Keep waveform phase/framing across the reconfiguration",
		CompletionKind::none,
		[](Options &options, const std::string &value) {
			if (value == "true" || value == "1")
				options.simulator_preserve_phase = true;
			else if (value == "false" || value == "0")
				options.simulator_preserve_phase = false;
			else
				throw std::invalid_argument(
					"--preserve-phase requires true or false");
		},
	});
	simulator.add_subcommand(std::move(simulator_configure));

	Command simulator_event(
		"event", "Arm, cancel, or inspect a simulator amplitude event",
		run_simulator_event,
		{
			.access = AccessLevel::operator_control,
			.side_effect = SideEffect::control,
			.supports_text = true,
			.supports_json = true,
			.variants = {
				{"--action query",
				 AccessLevel::diagnostic,
				 "Read the sequencer state without changing it"},
			},
		});
	/* A query changes nothing, so it is a diagnostic read; arming,
	 * cancelling, and clearing are operator control. */
	simulator_event.set_access_resolver([](const Options &options) {
		return options.event_action.value_or("query") == "query"
			? AccessLevel::diagnostic
			: AccessLevel::operator_control;
	});
	simulator_event.add_option({
		"action", "ACTION", "arm, cancel, clear, or query",
		CompletionKind::none,
		[](Options &options, const std::string &value) {
			(void)event_action_value(value);
			options.event_action = value;
		},
	});
	simulator_event.add_option({
		"channels", "SPEC",
		"Lanes the envelope scales: voltage, current, all, or a "
		"comma-separated lane list (ia, ib, ic, in, vc, vb, va)",
		CompletionKind::none,
		[](Options &options, const std::string &value) {
			(void)event_channel_mask(value);
			options.event_channels = value;
		},
	});
	simulator_event.add_option({
		"scale-percent", "PERCENT",
		"Amplitude during the burst: 100 unity, 0 interruption",
		CompletionKind::none,
		[](Options &options, const std::string &value) {
			const auto parsed =
				parse_finite_double(value, "--scale-percent");
			if (parsed < 0.0 || parsed > 400.0)
				throw std::invalid_argument(
					"--scale-percent must be 0..400");
			options.event_scale_percent = parsed;
		},
	});
	simulator_event.add_option({
		"duration", "HALFCYC",
		"Burst length in half cycles (Urms(1/2) refreshes every half "
		"cycle)",
		CompletionKind::none,
		[](Options &options, const std::string &value) {
			options.event_duration_half_cycles =
				parse_half_cycles(value, "--duration");
		},
	});
	simulator_event.add_option({
		"period", "HALFCYC",
		"Repeat period in half cycles, measured start to start",
		CompletionKind::none,
		[](Options &options, const std::string &value) {
			options.event_period_half_cycles =
				parse_half_cycles(value, "--period");
		},
	});
	simulator_event.add_option({
		"repeat", "BOOL", "Re-fire the burst on its period",
		CompletionKind::none,
		[](Options &options, const std::string &value) {
			if (value == "true" || value == "1")
				options.event_repeat = true;
			else if (value == "false" || value == "0")
				options.event_repeat = false;
			else
				throw std::invalid_argument(
					"--repeat requires true or false");
		},
	});
	simulator.add_subcommand(std::move(simulator_event));
	adc.add_subcommand(std::move(simulator));
	adc.add_subcommand(Command(
		"start", "Start ADC capture and meter acquisition",
		[](const Options &options, std::ostream &output) {
			return run_control(options, output, true);
		},
		{
			.access = AccessLevel::operator_control,
			.side_effect = SideEffect::control,
			.supports_text = true,
			.supports_json = true,
			.variants = {},
		}));
	adc.add_subcommand(Command(
		"stop", "Stop ADC capture and meter acquisition",
		[](const Options &options, std::ostream &output) {
			return run_control(options, output, false);
		},
		{
			.access = AccessLevel::operator_control,
			.side_effect = SideEffect::control,
			.supports_text = true,
			.supports_json = true,
			.variants = {},
		}));
	Command rate(
		"rate", "Inspect or temporarily set the ADC sample rate",
		run_rate,
		{
			.access = AccessLevel::diagnostic,
			.side_effect = SideEffect::none,
			.supports_text = true,
			.supports_json = true,
			.variants = {
				{"--sps RATE", AccessLevel::operator_control,
				 "Temporarily reconfigure the acquisition rate"},
			},
		});
	rate.set_access_resolver([](const Options &options) {
		return options.sample_rate_hz ? AccessLevel::operator_control
					      : AccessLevel::diagnostic;
	});
	rate.add_option({
		"sps", "RATE", "Temporary sample rate in frames per second",
		CompletionKind::none,
		[](Options &options, const std::string &value) {
			options.sample_rate_hz = parse_sample_rate(value);
		},
	});
	adc.add_subcommand(std::move(rate));
	Command test_flow(
		"testflw",
		"Run a destructive, self-restoring ADC diagnostic flow",
		run_test_flow,
		{
			.access = AccessLevel::maintenance,
			.side_effect = SideEffect::destructive_diagnostic,
			.supports_text = true,
			.supports_json = false,
			.variants = {},
		});
	test_flow.add_option({
		"flow", "NUMBER", "Diagnostic flow (currently: 1)",
		CompletionKind::none,
		[](Options &options, const std::string &value) {
			options.diagnostic_flow = parse_diagnostic_flow(value);
		},
	});
	adc.add_subcommand(std::move(test_flow));
	application.add_command(std::move(adc));
}

} // namespace msap1::cli
