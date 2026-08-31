#include "config/runtime_config.hpp"

#include "msap1/settings/settings_ipc.hpp"
#include "msap1/system/system_identity.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>
#include <numeric>
#include <sstream>
#include <stdexcept>

namespace msap1::acquisition::daemon {
namespace {

std::uint8_t hex_digit(char value)
{
	if (value >= '0' && value <= '9')
		return static_cast<std::uint8_t>(value - '0');
	if (value >= 'a' && value <= 'f')
		return static_cast<std::uint8_t>(value - 'a' + 10);
	if (value >= 'A' && value <= 'F')
		return static_cast<std::uint8_t>(value - 'A' + 10);
	throw std::invalid_argument("invalid SHA-256 text");
}

msap1::MncwfSha256 digest_from_hex(std::string_view text)
{
	if (text.size() != 64u)
		throw std::invalid_argument("SHA-256 text must contain 64 digits");
	msap1::MncwfSha256 result{};
	for (std::size_t index = 0; index < result.size(); ++index)
		result[index] = static_cast<std::byte>(
			(hex_digit(text[index * 2u]) << 4u) |
			hex_digit(text[index * 2u + 1u]));
	return result;
}

msap1::MncwfUuid uuid_from_digest(const msap1::MncwfSha256 &digest)
{
	msap1::MncwfUuid uuid{};
	std::copy_n(digest.begin(), uuid.size(), uuid.begin());
	/* Name-derived UUID, RFC-4122 variant/version-5 layout. */
	uuid[6] = static_cast<std::byte>(
		(std::to_integer<std::uint8_t>(uuid[6]) & 0x0fu) | 0x50u);
	uuid[8] = static_cast<std::byte>(
		(std::to_integer<std::uint8_t>(uuid[8]) & 0x3fu) | 0x80u);
	return uuid;
}

std::string machine_identity()
{
	std::ifstream input("/etc/machine-id");
	std::string value;
	input >> value;
	if (value.empty())
		throw std::runtime_error(
			"MNCWF v4 requires a provisioned /etc/machine-id");
	return value;
}

std::pair<std::uint64_t, std::uint64_t> decimal_ratio(
	double numerator, double denominator)
{
	constexpr double scale = 1'000'000.0;
	const auto left = static_cast<std::uint64_t>(std::llround(numerator * scale));
	const auto right = static_cast<std::uint64_t>(std::llround(denominator * scale));
	if (left == 0u || right == 0u)
		return {0u, 0u};
	const auto divisor = std::gcd(left, right);
	return {left / divisor, right / divisor};
}

msap1::MncwfCalibrationStatus calibration_status(std::string_view value)
{
	if (value == "valid")
		return msap1::MncwfCalibrationStatus::valid;
	if (value == "expired")
		return msap1::MncwfCalibrationStatus::expired;
	if (value == "invalid")
		return msap1::MncwfCalibrationStatus::invalid;
	return msap1::MncwfCalibrationStatus::unknown;
}

std::int32_t local_utc_offset_seconds()
{
	const auto now = std::time(nullptr);
	std::tm local{};
	if (::localtime_r(&now, &local) == nullptr)
		return 0;
	return static_cast<std::int32_t>(local.tm_gmtoff);
}

} // namespace

msap1::settings::ProductSettings load_runtime_settings()
{
	msap1::settings::ipc::SettingsClient client;
	return client.active(5000);
}

msap1::WaveformCaptureContext waveform_capture_context(
	const msap1::settings::ProductSettings &settings,
	const msap1::PreparedMeterConfiguration &configuration)
{
	msap1::WaveformCaptureContext result{};
	auto actual_settings = settings;
	actual_settings.metering.sample_rate_hz = configuration.wire.sample_rate_hz;
	actual_settings.adc.source = configuration.source.adc_source;
	const auto canonical = msap1::settings::SettingsCodec::encode(
		actual_settings, false);
	const auto configuration_hash =
		msap1::settings::SettingsCodec::hash(canonical);
	const auto sensor_document =
		msap1::encode_meter_configuration(configuration.source, false);
	const auto sensor_hash =
		msap1::settings::SettingsCodec::hash(sensor_document);
	const auto machine = machine_identity();
	const auto device_digest = msap1::mncwf_sha256("msap1-device:" + machine);
	const auto image = msap1::read_image_identity();
	const auto &waveform = actual_settings.waveform;
	auto &capture = result.capture_metadata;
	capture.device_uuid = uuid_from_digest(device_digest);
	capture.configuration_sha256 = digest_from_hex(configuration_hash);
	capture.sensor_profile_sha256 = digest_from_hex(sensor_hash);
	capture.nominal_voltage_numerator = static_cast<std::int64_t>(std::llround(
		actual_settings.metering.system_nominal_voltage_v * 1'000'000.0));
	capture.nominal_voltage_denominator = 1'000'000u;
	capture.nominal_frequency_numerator =
		actual_settings.metering.nominal_frequency_hz;
	capture.nominal_frequency_denominator = 1u;
	capture.topology = actual_settings.metering.measurement_topology == "delta"
		? msap1::MncwfTopology::delta : msap1::MncwfTopology::wye;
	capture.calibration_status = calibration_status(waveform.calibration_status);
	capture.station_name = waveform.station_name.empty()
		? waveform.station_id : waveform.station_name;
	capture.site_name = waveform.site_name.empty()
		? waveform.site_id : waveform.site_name;
	capture.circuit_name = waveform.circuit_name.empty()
		? waveform.circuit_id : waveform.circuit_name;
	capture.product_name = "MSAP1";
	capture.device_model = "MSAP1";
	capture.firmware_version = image.available && !image.distro_version.empty()
		? image.distro_version : "development";
	capture.software_build_id = image.available && !image.build_hash.empty()
		? image.build_hash : "development";
	capture.sensor_profile_id = configuration.source.profile_id;
	capture.configuration_id = configuration_hash + ":g" +
		std::to_string(configuration.wire.generation);
	capture.calibration_id = waveform.calibration_id;
	capture.device_serial = waveform.device_serial;
	capture.comments = "station_id=" + waveform.station_id +
		";site_id=" + waveform.site_id + ";circuit_id=" +
		waveform.circuit_id + ";adc_source=" +
		configuration.source.adc_source;

	static constexpr std::array phases{
		msap1::MncwfPhase::a, msap1::MncwfPhase::b,
		msap1::MncwfPhase::c, msap1::MncwfPhase::neutral,
		msap1::MncwfPhase::c, msap1::MncwfPhase::b,
		msap1::MncwfPhase::a,
	};
	constexpr std::int64_t raw_low = -(1ll << 23u);
	constexpr std::int64_t raw_high = (1ll << 23u) - 1ll;
	constexpr std::uint64_t engineering_denominator =
		65'536ull * 1'000'000ull;
	for (std::uint32_t source = 0; source < waveform_persisted_channels;
	     ++source) {
		if ((configuration.wire.valid_mask & (1u << source)) == 0u)
			continue;
		MncwfV4ChannelDefinition channel{};
		channel.stable_id = uuid_from_digest(msap1::mncwf_sha256(
			"msap1-channel:" + machine + ":" + std::to_string(source)));
		channel.source_channel = source;
		channel.flags = mncwf_channel_enabled |
			mncwf_channel_transform_valid | mncwf_channel_nominal_valid |
			mncwf_channel_range_valid | mncwf_channel_resolution_valid |
			mncwf_channel_clipping_valid;
		if (capture.calibration_status == MncwfCalibrationStatus::valid)
			channel.flags |= mncwf_channel_calibration_valid;
		channel.phase = phases[source];
		channel.quantity = source < 4u ? MncwfQuantity::current
			: MncwfQuantity::voltage;
		channel.si_unit = source < 4u ? MncwfSiUnit::ampere
			: MncwfSiUnit::volt;
		channel.storage_bits = 32u;
		channel.valid_bits = 24u;
		channel.display_exponent10 = -6;
		channel.gain_numerator =
			configuration.wire.scale_micro_units_q16[source];
		channel.gain_denominator = engineering_denominator;
		channel.offset_denominator = 1u;
		channel.range_minimum_numerator = raw_low * channel.gain_numerator;
		channel.range_minimum_denominator = engineering_denominator;
		channel.range_maximum_numerator = raw_high * channel.gain_numerator;
		channel.range_maximum_denominator = engineering_denominator;
		channel.resolution_numerator = static_cast<std::uint64_t>(
			channel.gain_numerator);
		channel.resolution_denominator = engineering_denominator;
		channel.clipping_low = raw_low;
		channel.clipping_high = raw_high;
		if (source < 4u) {
			const auto found = std::ranges::find_if(
				configuration.source.current_channels,
				[source](const auto &value) { return value.channel == source; });
			if (found == configuration.source.current_channels.end())
				throw std::runtime_error("missing waveform current channel");
			channel.name = found->name;
			channel.unit_symbol = "A";
			channel.description = found->sensor_model;
			channel.nominal_numerator = static_cast<std::int64_t>(std::llround(
				found->primary_rated_amps * 1'000'000.0));
			channel.nominal_denominator = 1'000'000u;
			if (found->sensor_model == "internal_ct") {
				const auto ratio = decimal_ratio(found->primary_rated_amps,
					found->secondary_rated_amps);
				channel.primary_secondary_ratio_numerator = ratio.first;
				channel.primary_secondary_ratio_denominator = ratio.second;
				channel.flags |= mncwf_channel_ratio_valid;
			}
		} else {
			const auto found = std::ranges::find_if(
				configuration.source.voltage_channels,
				[source](const auto &value) { return value.channel == source; });
			if (found == configuration.source.voltage_channels.end())
				throw std::runtime_error("missing waveform voltage channel");
			channel.name = found->name;
			channel.unit_symbol = "V";
			channel.description = "resistive voltage frontend";
			channel.nominal_numerator = capture.nominal_voltage_numerator;
			channel.nominal_denominator = capture.nominal_voltage_denominator;
			const auto ratio = decimal_ratio(found->rin_ohms, found->rf_ohms);
			channel.primary_secondary_ratio_numerator = ratio.first;
			channel.primary_secondary_ratio_denominator = ratio.second;
			channel.flags |= mncwf_channel_ratio_valid;
		}
		result.channels.push_back(std::move(channel));
	}
	result.clock_source = MncwfClockSource::system;
	result.time_quality = MncwfTimeQuality::unlocked;
	result.time_flags = mncwf_time_utc_offset_known;
	result.utc_offset_seconds = local_utc_offset_seconds();
	return result;
}

msap1::FrequencyIpcConfiguration frequency_ipc(
	const msap1::FrequencyConfig &frequency)
{
	std::uint32_t mode = MSAP1_FREQUENCY_MODE_ROLLING_CYCLES;
	if (frequency.mode == "single_cycle")
		mode = MSAP1_FREQUENCY_MODE_SINGLE_CYCLE;
	else if (frequency.mode == "rolling_time")
		mode = MSAP1_FREQUENCY_MODE_ROLLING_TIME;
	return {
		frequency.enabled ? 1u : 0u,
		frequency.reference_channel,
		mode,
		frequency.averaging_cycles,
		frequency.averaging_window_ms,
		static_cast<std::uint32_t>(
			std::llround(frequency.minimum_hz * 1000.0)),
		static_cast<std::uint32_t>(
			std::llround(frequency.maximum_hz * 1000.0)),
		static_cast<std::uint32_t>(
			std::llround(frequency.hysteresis_volts * 1000000.0)),
	};
}

msap1::SimulatorIpcConfiguration simulator_ipc(
	const msap1::SimulatorConfig &simulator)
{
	msap1::SimulatorIpcConfiguration result{};
	result.frequency_millihz = static_cast<std::uint32_t>(
		std::llround(simulator.frequency_hz * 1000.0));
	result.preserve_phase = simulator.preserve_phase ? 1u : 0u;
	for (const auto &channel : simulator.channels) {
		if (channel.channel >= result.channels.size())
			continue;
		result.channels[channel.channel].rms = channel.rms;
		result.channels[channel.channel].phase_degrees =
			channel.phase_degrees;
		result.channels[channel.channel].dc = channel.dc;
		result.channels[channel.channel].noise_rms = channel.noise_rms;
	}
	result.harmonics.reserve(simulator.harmonics.size());
	for (const auto &harmonic : simulator.harmonics)
		result.harmonics.push_back({harmonic.order, harmonic.percent,
					    harmonic.phase_degrees,
					    harmonic.channels});
	return result;
}

} // namespace msap1::acquisition::daemon
