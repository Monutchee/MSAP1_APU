#include "msap1/settings/definition/data_logging_settings.hpp"

#include "mnc/MeterDataProvider/attributes/meter_attribute.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace msap1::settings {
namespace {

using Calculation = mnc::meter::MeterAttributeCalculation;
using Period = mnc::meter::MeasurementPeriod;

bool has_control(std::string_view value)
{
	return std::ranges::any_of(value, [](unsigned char character) {
		return std::iscntrl(character) != 0;
	});
}

void validate_label(std::string_view value, std::string_view description)
{
	if (value.empty() || value.size() > 128 || has_control(value))
		throw std::runtime_error(std::string(description) + " is invalid");
}

void validate_host(std::string_view host)
{
	if (host.empty() || host.size() > 253 || has_control(host) ||
	    host.find_first_of(" /\\?#@") != std::string_view::npos ||
	    host.contains("://"))
		throw std::runtime_error("data channel host is invalid");
}

void validate_absolute_path(std::string_view path,
	std::string_view description)
{
	if (path.empty() || path.size() > 1024 || path.front() != '/' ||
	    has_control(path) || path.find('\\') != std::string_view::npos ||
	    path.find_first_of(" ?") != std::string_view::npos ||
	    path.find('#') != std::string_view::npos)
		throw std::runtime_error(std::string(description) + " is invalid");
	std::size_t begin = 1;
	while (begin <= path.size()) {
		const auto end = path.find('/', begin);
		const auto component = path.substr(begin, end - begin);
		if (component == "." || component == "..")
			throw std::runtime_error(std::string(description) +
				" contains path traversal");
		if (end == std::string_view::npos)
			break;
		begin = end + 1;
	}
}

std::optional<Period> period_from_key(std::string_view key)
{
	for (const auto &candidate : mnc::meter::defined_measurement_periods())
		if (candidate.key == key && candidate.historian)
			return candidate.period;
	return std::nullopt;
}

std::optional<Calculation> calculation_from_key(std::string_view key)
{
	static constexpr std::array values{
		std::pair{std::string_view{"minimum"}, Calculation::Minimum},
		std::pair{std::string_view{"maximum"}, Calculation::Maximum},
		std::pair{std::string_view{"average"}, Calculation::Average},
		std::pair{std::string_view{"last"}, Calculation::Last},
		std::pair{std::string_view{"circular_average"},
			Calculation::CircularAverage},
		std::pair{std::string_view{"first"}, Calculation::First},
		std::pair{std::string_view{"delta"}, Calculation::Delta},
	};
	for (const auto &[name, value] : values)
		if (name == key)
			return value;
	return std::nullopt;
}

bool supports_calculation(const mnc::meter::MeterAttributeDescriptor &attribute,
	Calculation calculation)
{
	return std::ranges::find(attribute.calculations, calculation) !=
		attribute.calculations.end();
}

std::uint64_t nominal_period_seconds(Period period,
	std::uint32_t demand_window_seconds)
{
	switch (period) {
	case Period::Basic: return 1; // settings durations are whole seconds
	case Period::Seconds10: return 10;
	case Period::Cycles150_180: return 3;
	case Period::Min10: return 600;
	case Period::Hour2: return 7200;
	case Period::Demand: return demand_window_seconds;
	case Period::Min10Live:
	case Period::Hour2Live: break;
	}
	throw std::runtime_error("open-interval period cannot generate data logs");
}

void validate_channel(const DataChannelSettings &channel)
{
	if (!valid_data_channel_id(channel.id))
		throw std::runtime_error("data channel ID must be a UUID");
	validate_label(channel.name, "data channel name");
	validate_host(channel.host);
	if (channel.connect_timeout_seconds == 0 ||
	    channel.connect_timeout_seconds > 300 ||
	    channel.transfer_timeout_seconds == 0 ||
	    channel.transfer_timeout_seconds > 3600)
		throw std::runtime_error("data channel timeout is invalid");
	if (has_control(channel.username) || channel.username.size() > 256)
		throw std::runtime_error("data channel username is invalid");

	using Protocol = DataChannelProtocol;
	using Auth = DataChannelAuthentication;
	if (channel.protocol == Protocol::http ||
	    channel.protocol == Protocol::https) {
		validate_absolute_path(channel.http_path, "HTTP request path");
		if (channel.authentication != Auth::none &&
		    channel.authentication != Auth::basic &&
		    channel.authentication != Auth::bearer &&
		    channel.authentication != Auth::mtls)
			throw std::runtime_error("authentication is unsupported by HTTP");
		if (channel.authentication == Auth::basic && channel.username.empty())
			throw std::runtime_error("HTTP Basic username is required");
		if (channel.authentication == Auth::mtls &&
		    !channel.use_client_certificate)
			throw std::runtime_error("mTLS requires a client certificate");
	} else {
		validate_absolute_path(channel.remote_directory,
			"remote directory");
		if (channel.authentication != Auth::password &&
		    !(channel.protocol == Protocol::sftp &&
		      channel.authentication == Auth::private_key))
			throw std::runtime_error(
				"FTP/SFTP requires password or private-key authentication");
		if (channel.username.empty())
			throw std::runtime_error("FTP/SFTP username is required");
	}

	if ((channel.protocol == Protocol::http ||
	     channel.protocol == Protocol::ftp) &&
	    !channel.insecure_transport_acknowledged)
		throw std::runtime_error(
			"HTTP/FTP requires an insecure transport acknowledgement");
	if (channel.protocol == Protocol::https) {
		if (channel.use_system_ca == channel.use_uploaded_ca)
			throw std::runtime_error(
				"HTTPS must select exactly one trusted CA source");
	} else if (channel.use_uploaded_ca || channel.use_client_certificate ||
		   !channel.use_system_ca)
		throw std::runtime_error(
			"TLS options are valid only for HTTPS channels");
}

} // namespace

bool valid_data_channel_id(std::string_view value) noexcept
{
	if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
	    value[18] != '-' || value[23] != '-')
		return false;
	for (std::size_t index = 0; index < value.size(); ++index) {
		if (index == 8 || index == 13 || index == 18 || index == 23)
			continue;
		const auto character = static_cast<unsigned char>(value[index]);
		if (!std::isxdigit(character))
			return false;
	}
	return true;
}

void DataLoggingSettings::validate(
	std::uint32_t demand_window_seconds) const
{
	if (channels.size() > 64 || jobs.size() > 64)
		throw std::runtime_error("too many data logging channels or jobs");
	if (storage.maximum_bytes < 1024u * 1024u ||
	    storage.minimum_free_bytes < 1024u * 1024u ||
	    storage.completed_metadata_retention_days == 0 ||
	    storage.completed_metadata_retention_days > 3650)
		throw std::runtime_error("data logging storage policy is invalid");

	std::unordered_map<std::string_view, const DataChannelSettings *> channel_by_id;
	for (const auto &channel : channels) {
		validate_channel(channel);
		if (!channel_by_id.emplace(channel.id, &channel).second)
			throw std::runtime_error("data channel IDs must be unique");
	}

	std::unordered_set<std::string_view> job_ids;
	for (const auto &job : jobs) {
		if (!valid_data_channel_id(job.id) || !job_ids.insert(job.id).second)
			throw std::runtime_error("data logging job IDs must be unique UUIDs");
		validate_label(job.name, "data logging job name");
		if (job.revision == 0)
			throw std::runtime_error("data logging job revision must be nonzero");
		const auto period = period_from_key(job.source_period);
		if (!period)
			throw std::runtime_error("unknown historian source period");
		if (job.generation_interval_seconds == 0 ||
		    job.generation_interval_seconds > 31u * 24u * 60u * 60u ||
		    job.row_interval_seconds == 0 ||
		    job.generation_interval_seconds % job.row_interval_seconds != 0)
			throw std::runtime_error(
				"generation interval must be divisible by row interval");
		const auto nominal = nominal_period_seconds(
			*period, demand_window_seconds);
		if (job.row_interval_seconds < nominal ||
		    job.row_interval_seconds % nominal != 0)
			throw std::runtime_error(
				"row interval is incompatible with the source period");
		if (job.format != "json" && job.format != "csv")
			throw std::runtime_error("data logging format must be json or csv");
		if (job.enabled && job.selections.empty())
			throw std::runtime_error("enabled data logging job has no columns");
		if (job.selections.size() > 512)
			throw std::runtime_error("too many data logging columns");

		std::unordered_set<std::string> columns;
		for (const auto &selection : job.selections) {
			const auto attribute = mnc::meter::find_attribute(selection.attribute);
			const auto calculation = calculation_from_key(selection.calculation);
			if (!attribute || !calculation || attribute->index ||
			    !mnc::meter::supports_attribute(*attribute, *period,
				    mnc::meter::MeterAttributeUsage::Historian) ||
			    !supports_calculation(mnc::meter::describe(*attribute),
				    *calculation))
				throw std::runtime_error(
					"unsupported data logging attribute/calculation");
			if (!columns.insert(selection.attribute + ":" +
				selection.calculation).second)
				throw std::runtime_error("duplicate data logging column");
		}

		std::unordered_set<std::string_view> selected_channels;
		if (job.destination == DataLoggingDestination::local_only) {
			if (!job.channel_ids.empty())
				throw std::runtime_error(
					"Local-only jobs cannot select remote channels");
		} else {
			if (job.enabled && job.channel_ids.empty())
				throw std::runtime_error(
					"enabled remote job has no selected channel");
			for (const auto &id : job.channel_ids) {
				const auto channel = channel_by_id.find(id);
				if (channel == channel_by_id.end() ||
				    !selected_channels.insert(id).second)
					throw std::runtime_error(
						"data logging job references an unknown or duplicate channel");
				if (job.enabled && !channel->second->enabled)
					throw std::runtime_error(
						"enabled job references a disabled data channel");
			}
		}
	}
}

} // namespace msap1::settings
