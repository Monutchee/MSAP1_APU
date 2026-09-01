#include "mnc/datalogger/transfer.hpp"

#include <algorithm>
#include <cctype>
#include <ranges>
#include <stdexcept>

namespace mnc::datalogger {
namespace {

std::string_view protocol_name(OutboundProtocol protocol)
{
	switch (protocol) {
	case OutboundProtocol::Http: return "http";
	case OutboundProtocol::Https: return "https";
	case OutboundProtocol::Ftp: return "ftp";
	case OutboundProtocol::Sftp: return "sftp";
	}
	return "unknown";
}

std::uint16_t default_port(OutboundProtocol protocol)
{
	switch (protocol) {
	case OutboundProtocol::Http: return 80;
	case OutboundProtocol::Https: return 443;
	case OutboundProtocol::Ftp: return 21;
	case OutboundProtocol::Sftp: return 22;
	}
	return 0;
}

bool safe_identity(std::string_view value)
{
	return !value.empty() && value.size() <= 160 &&
		std::ranges::all_of(value, [](unsigned char character) {
			return std::isalnum(character) != 0 || character == '-' ||
				character == '_';
		});
}

bool safe_host(std::string_view value)
{
	return !value.empty() && value.size() <= 253 &&
		value.find_first_of(" /\\?#@\r\n\t") == std::string_view::npos &&
		!value.contains("://");
}

bool safe_path(std::string_view value)
{
	if (value.empty() || value.size() > 1024 || value.front() != '/' ||
	    value.find_first_of("\\?#\r\n\t ") != std::string_view::npos)
		return false;
	std::size_t start = 1;
	while (start <= value.size()) {
		const auto end = value.find('/', start);
		const auto component = value.substr(start, end - start);
		if (component == "." || component == "..")
			return false;
		if (end == std::string_view::npos)
			break;
		start = end + 1;
	}
	return true;
}

std::string join_path(std::string_view directory, std::string_view filename)
{
	return std::string(directory) + (directory.ends_with('/') ? "" : "/") +
		std::string(filename);
}

std::string endpoint(const RuntimeDataChannelConfiguration &configuration,
	std::string_view path)
{
	const auto port = configuration.port == 0
		? default_port(configuration.protocol) : configuration.port;
	return std::string(protocol_name(configuration.protocol)) + "://" +
		configuration.host + ":" + std::to_string(port) + std::string(path);
}

void validate(const RuntimeDataChannelConfiguration &configuration)
{
	if (!safe_identity(configuration.id) || !safe_host(configuration.host) ||
	    configuration.connect_timeout_seconds == 0 ||
	    configuration.transfer_timeout_seconds == 0)
		throw std::invalid_argument("runtime data channel is invalid");
	if ((configuration.protocol == OutboundProtocol::Http ||
	     configuration.protocol == OutboundProtocol::Https) &&
	    !safe_path(configuration.http_path))
		throw std::invalid_argument("runtime HTTP path is invalid");
	if ((configuration.protocol == OutboundProtocol::Ftp ||
	     configuration.protocol == OutboundProtocol::Sftp) &&
	    !safe_path(configuration.remote_directory))
		throw std::invalid_argument("runtime remote directory is invalid");
	if (configuration.protocol == OutboundProtocol::Https) {
		if (configuration.use_system_ca == configuration.ca_file.has_value())
			throw std::invalid_argument(
				"HTTPS runtime must select one CA source");
		if (configuration.authentication == RuntimeAuthentication::MutualTls &&
		    (!configuration.client_certificate_file ||
		     !configuration.client_key_file))
			throw std::invalid_argument("mTLS runtime assets are missing");
	}
	if (configuration.protocol == OutboundProtocol::Sftp &&
	    !configuration.known_hosts_file)
		throw std::invalid_argument("SFTP known-host verification is required");
	if (configuration.authentication == RuntimeAuthentication::PrivateKey &&
	    !configuration.sftp_private_key_file)
		throw std::invalid_argument("SFTP private key is missing");
}

} // namespace

ConfiguredDataChannel::ConfiguredDataChannel(
	RuntimeDataChannelConfiguration configuration, TransferClient &transfer)
	: configuration_(std::move(configuration)), transfer_(transfer)
{
	validate(configuration_);
}

std::string_view ConfiguredDataChannel::protocol() const noexcept
{
	return protocol_name(configuration_.protocol);
}

DeliveryResult ConfiguredDataChannel::deliver(const DeliveryRequest &request)
{
	if (request.channel_id != configuration_.id ||
	    !safe_identity(request.content.artifact_id) ||
	    request.content.filename != request.content.artifact_id + "." +
		request.content.extension)
		return {DeliveryDisposition::Blocked, {},
			"delivery request identity is invalid"};
	TransferPlan plan;
	plan.protocol = configuration_.protocol;
	plan.filename = request.content.filename;
	plan.artifact_id = request.content.artifact_id;
	plan.checksum_sha256 = request.zero_data_probe
		? "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
		: request.content.sha256;
	plan.mime_type = request.content.mime_type;
	plan.body = request.zero_data_probe ? std::string{} : request.content.body;
	plan.authentication = configuration_.authentication;
	plan.username = configuration_.username;
	plan.password = configuration_.password;
	plan.bearer_token = configuration_.bearer_token;
	plan.private_key_passphrase = configuration_.private_key_passphrase;
	plan.connect_timeout_seconds = configuration_.connect_timeout_seconds;
	plan.transfer_timeout_seconds = configuration_.transfer_timeout_seconds;
	plan.use_system_ca = configuration_.use_system_ca;
	plan.ca_file = configuration_.ca_file;
	plan.client_certificate_file = configuration_.client_certificate_file;
	plan.client_key_file = configuration_.client_key_file;
	plan.sftp_private_key_file = configuration_.sftp_private_key_file;
	plan.known_hosts_file = configuration_.known_hosts_file;
	plan.zero_data_probe = request.zero_data_probe;

	if (configuration_.protocol == OutboundProtocol::Http ||
	    configuration_.protocol == OutboundProtocol::Https) {
		plan.url = endpoint(configuration_, configuration_.http_path);
		plan.headers = {
			"Content-Type: " + request.content.mime_type,
			"X-MNC-Filename: " + request.content.filename,
			"X-MNC-Artifact-ID: " + request.content.artifact_id,
			"X-MNC-SHA256: " + plan.checksum_sha256,
			"Idempotency-Key: " + request.content.artifact_id,
			std::string("X-MNC-Zero-Data-Probe: ") +
				(request.zero_data_probe ? "true" : "false"),
		};
	} else {
		const auto temporary = "." + request.content.filename + ".part";
		plan.remote_temporary_path = join_path(
			configuration_.remote_directory, temporary);
		plan.remote_final_path = join_path(
			configuration_.remote_directory, request.content.filename);
		plan.url = endpoint(configuration_, *plan.remote_temporary_path);
	}
	return classify_transfer_result(configuration_.protocol,
		transfer_.perform(plan));
}

DeliveryResult classify_transfer_result(OutboundProtocol protocol,
	const TransferOutcome &outcome)
{
	if (!outcome.transport_succeeded) {
		const auto disposition = outcome.failure == TransferFailure::Transient
			? DeliveryDisposition::Retryable : DeliveryDisposition::Blocked;
		return {disposition, {}, outcome.sanitized_message};
	}
	if (protocol != OutboundProtocol::Http &&
	    protocol != OutboundProtocol::Https)
		return {DeliveryDisposition::Succeeded,
			std::to_string(outcome.response_code), {}};
	if (outcome.response_code >= 200 && outcome.response_code < 300)
		return {DeliveryDisposition::Succeeded,
			std::to_string(outcome.response_code), {}};
	if (outcome.response_code == 408 || outcome.response_code == 429 ||
	    outcome.response_code >= 500)
		return {DeliveryDisposition::Retryable,
			std::to_string(outcome.response_code),
			"remote HTTP service asked for a retry"};
	return {DeliveryDisposition::Blocked,
		std::to_string(outcome.response_code),
		"remote HTTP service rejected the artifact"};
}

} // namespace mnc::datalogger
