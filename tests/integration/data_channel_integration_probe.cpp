#include "mnc/datalogger/transfer.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using namespace mnc::datalogger;

std::string required_environment(const char *name)
{
	const auto *value = std::getenv(name);
	if (!value || *value == '\0')
		throw std::runtime_error(std::string("missing environment: ") + name);
	return value;
}

std::uint16_t port(const char *name)
{
	const auto text = required_environment(name);
	const auto value = std::stoul(text);
	if (value == 0 || value > 65535)
		throw std::runtime_error(std::string("invalid port: ") + name);
	return static_cast<std::uint16_t>(value);
}

void require(bool condition, std::string_view message)
{
	if (!condition)
		throw std::runtime_error(std::string(message));
}

GeneratedContent content(std::string artifact_id = "integration-artifact")
{
	return {
		.artifact_id = artifact_id,
		.filename = artifact_id + ".json",
		.mime_type = "application/json",
		.extension = "json",
		.body = "{\"meter\":\"m19\",\"value\":0}\n",
		.sha256 = "0f7038b00f6dbf5bf5431256a349f2d3d37e082e9d519223f2f62586a4bbc5bb",
	};
}

DeliveryResult send(RuntimeDataChannelConfiguration configuration,
	const GeneratedContent &payload)
{
	CurlTransferClient transfer;
	ConfiguredDataChannel channel(configuration, transfer);
	return channel.deliver({configuration.id, payload, false});
}

RuntimeDataChannelConfiguration http_configuration(
	OutboundProtocol protocol, std::string host, std::uint16_t selected_port,
	std::string path)
{
	RuntimeDataChannelConfiguration result;
	result.id = protocol == OutboundProtocol::Http ? "http-channel" :
		"https-channel";
	result.protocol = protocol;
	result.host = std::move(host);
	result.port = selected_port;
	result.http_path = std::move(path);
	result.connect_timeout_seconds = 3;
	result.transfer_timeout_seconds = 10;
	return result;
}

void verify_http()
{
	const auto selected_port = port("M19_HTTP_PORT");
	auto configuration = http_configuration(
		OutboundProtocol::Http, "127.0.0.1", selected_port, "/accepted");
	require(send(configuration, content()).disposition ==
		DeliveryDisposition::Succeeded, "real HTTP POST did not succeed");

	configuration.http_path = "/retry";
	require(send(configuration, content()).disposition ==
		DeliveryDisposition::Retryable, "HTTP 503 was not retryable");

	configuration.http_path = "/blocked";
	require(send(configuration, content()).disposition ==
		DeliveryDisposition::Blocked, "HTTP 401 was not blocked");

	configuration.http_path = "/drop";
	require(send(configuration, content()).disposition ==
		DeliveryDisposition::Retryable,
		"interrupted HTTP response was not retryable");
}

void verify_https()
{
	const auto selected_port = port("M19_HTTPS_PORT");
	auto configuration = http_configuration(
		OutboundProtocol::Https, "localhost", selected_port, "/accepted");
	configuration.use_system_ca = false;
	configuration.ca_file = required_environment("M19_CA_FILE");
	configuration.authentication = RuntimeAuthentication::MutualTls;
	configuration.client_certificate_file =
		required_environment("M19_CLIENT_CERT_FILE");
	configuration.client_key_file = required_environment("M19_CLIENT_KEY_FILE");
	require(send(configuration, content()).disposition ==
		DeliveryDisposition::Succeeded, "real mTLS HTTPS POST did not succeed");

	configuration.ca_file = required_environment("M19_WRONG_CA_FILE");
	const auto untrusted = send(configuration, content());
	require(untrusted.disposition == DeliveryDisposition::Blocked &&
		!untrusted.sanitized_error.contains("integration-secret"),
		"untrusted HTTPS certificate was not safely blocked");

	configuration.ca_file = required_environment("M19_CA_FILE");
	configuration.host = "127.0.0.1";
	require(send(configuration, content()).disposition ==
		DeliveryDisposition::Blocked,
		"HTTPS hostname mismatch was not blocked");
}

void verify_ftp()
{
	RuntimeDataChannelConfiguration configuration;
	configuration.id = "ftp-channel";
	configuration.protocol = OutboundProtocol::Ftp;
	configuration.host = "127.0.0.1";
	configuration.port = port("M19_FTP_PORT");
	configuration.remote_directory = "/incoming";
	configuration.authentication = RuntimeAuthentication::Password;
	configuration.username = "meter";
	configuration.password = "integration-secret";
	configuration.connect_timeout_seconds = 3;
	configuration.transfer_timeout_seconds = 10;
	const auto uploaded = send(configuration, content());
	if (uploaded.disposition != DeliveryDisposition::Succeeded)
		throw std::runtime_error("real FTP upload and rename did not succeed: " +
			uploaded.remote_result + " " + uploaded.sanitized_error);

	configuration.password = "incorrect-secret";
	const auto denied = send(configuration, content("ftp-denied"));
	require(denied.disposition == DeliveryDisposition::Blocked &&
		!denied.sanitized_error.contains("incorrect-secret"),
		"FTP authentication failure was not safely blocked");

	configuration.password = "integration-secret";
	require(send(configuration, content("rename-fail")).disposition ==
		DeliveryDisposition::Blocked,
		"FTP final-rename rejection was not blocked");
}

void verify_sftp()
{
	RuntimeDataChannelConfiguration configuration;
	configuration.id = "sftp-channel";
	configuration.protocol = OutboundProtocol::Sftp;
	configuration.host = "127.0.0.1";
	configuration.port = port("M19_SFTP_PORT");
	configuration.remote_directory =
		required_environment("M19_SFTP_REMOTE_DIRECTORY");
	configuration.authentication = RuntimeAuthentication::PrivateKey;
	configuration.username = required_environment("M19_SFTP_USER");
	configuration.sftp_private_key_file =
		required_environment("M19_SFTP_PRIVATE_KEY_FILE");
	configuration.known_hosts_file =
		required_environment("M19_SFTP_KNOWN_HOSTS_FILE");
	configuration.connect_timeout_seconds = 3;
	configuration.transfer_timeout_seconds = 10;
	const auto uploaded = send(configuration, content());
	if (uploaded.disposition != DeliveryDisposition::Succeeded)
		throw std::runtime_error("real SFTP upload and rename did not succeed: " +
			uploaded.remote_result + " " + uploaded.sanitized_error);

	configuration.known_hosts_file =
		required_environment("M19_SFTP_WRONG_KNOWN_HOSTS_FILE");
	require(send(configuration, content("sftp-host-denied")).disposition ==
		DeliveryDisposition::Blocked,
		"SFTP host-key mismatch was not blocked");

	configuration.known_hosts_file =
		required_environment("M19_SFTP_KNOWN_HOSTS_FILE");
	configuration.sftp_private_key_file =
		required_environment("M19_SFTP_WRONG_PRIVATE_KEY_FILE");
	const auto denied = send(configuration, content("sftp-key-denied"));
	require(denied.disposition == DeliveryDisposition::Blocked &&
		!denied.sanitized_error.contains("integration-secret"),
		"SFTP authentication failure was not safely blocked");
}

} // namespace

int main()
{
	try {
		require(curl_transfer_available(),
			"integration probe was built without libcurl support");
		verify_http();
		verify_https();
		verify_ftp();
		verify_sftp();
		std::cout << "HTTP, mTLS HTTPS, FTP, and SFTP endpoint probes passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "data channel integration probe failed: " << error.what()
			<< '\n';
		return 1;
	}
}
