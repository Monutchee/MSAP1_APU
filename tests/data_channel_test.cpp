#include "mnc/datalogger/transfer.hpp"

#include <algorithm>
#include <stdexcept>

namespace {

using namespace mnc::datalogger;

void require(bool condition, std::string_view message)
{
	if (!condition)
		throw std::runtime_error(std::string(message));
}

class FakeTransfer final : public TransferClient {
public:
	TransferOutcome outcome{true, 204, TransferFailure::None, {}};
	std::optional<TransferPlan> last_plan;
	std::uint32_t call_count = 0;

	TransferOutcome perform(const TransferPlan &plan) override
	{
		++call_count;
		last_plan = plan;
		return outcome;
	}
};

GeneratedContent content()
{
	return {
		.artifact_id = "job-r1-0-300-json",
		.filename = "job-r1-0-300-json.json",
		.mime_type = "application/json",
		.extension = "json",
		.body = "{\"value\":1}\n",
		.sha256 = std::string(64, 'a'),
	};
}

void http_plan_carries_identity_and_never_embeds_secrets()
{
	FakeTransfer transfer;
	RuntimeDataChannelConfiguration configuration;
	configuration.id = "channel-a";
	configuration.protocol = OutboundProtocol::Https;
	configuration.host = "collector.example.test";
	configuration.http_path = "/api/meter-data";
	configuration.authentication = RuntimeAuthentication::Bearer;
	configuration.bearer_token = "do-not-expose";
	ConfiguredDataChannel channel(configuration, transfer);
	const auto generated = content();
	const auto result = channel.deliver({configuration.id, generated, false});
	require(result.disposition == DeliveryDisposition::Succeeded,
		"HTTP 204 was not accepted");
	require(transfer.last_plan &&
		transfer.last_plan->url ==
			"https://collector.example.test:443/api/meter-data" &&
		transfer.last_plan->body == generated.body,
		"HTTPS transfer plan lost its endpoint or body");
	require(transfer.last_plan->url.find("do-not-expose") == std::string::npos &&
		std::ranges::none_of(transfer.last_plan->headers,
			[](const auto &header) { return header.contains("do-not-expose"); }),
		"channel secret leaked into URL or persisted headers");
	const std::vector<std::string> expected_headers{
		"Content-Type: application/json",
		"X-MNC-Filename: job-r1-0-300-json.json",
		"X-MNC-Artifact-ID: job-r1-0-300-json",
		"X-MNC-SHA256: " + std::string(64, 'a'),
		"Idempotency-Key: job-r1-0-300-json"};
	for (const auto &expected : expected_headers)
		require(std::ranges::find(transfer.last_plan->headers, expected) !=
			transfer.last_plan->headers.end(),
			"HTTP artifact provenance header is missing");

	(void)channel.deliver({configuration.id, generated, true});
	require(transfer.last_plan->body.empty() &&
		std::ranges::find(transfer.last_plan->headers,
			"X-MNC-Zero-Data-Probe: true") !=
			transfer.last_plan->headers.end(),
		"saved-channel test was not clearly marked as zero data");
}

void http_result_classification_is_stable()
{
	for (const auto &[status, expected] : {
		std::pair{200L, DeliveryDisposition::Succeeded},
		std::pair{299L, DeliveryDisposition::Succeeded},
		std::pair{400L, DeliveryDisposition::Blocked},
		std::pair{408L, DeliveryDisposition::Retryable},
		std::pair{429L, DeliveryDisposition::Retryable},
		std::pair{503L, DeliveryDisposition::Retryable}}) {
		const auto result = classify_transfer_result(OutboundProtocol::Https,
			{true, status, TransferFailure::None, {}});
		require(result.disposition == expected,
			"HTTP status classification changed");
	}
	for (const auto &[failure, expected] : {
		std::pair{TransferFailure::Transient,
			DeliveryDisposition::Retryable},
		std::pair{TransferFailure::Authentication,
			DeliveryDisposition::Blocked},
		std::pair{TransferFailure::Verification,
			DeliveryDisposition::Blocked},
		std::pair{TransferFailure::Configuration,
			DeliveryDisposition::Blocked}}) {
		const auto result = classify_transfer_result(OutboundProtocol::Sftp,
			{false, 0, failure, "safe error"});
		require(result.disposition == expected &&
			result.sanitized_error == "safe error",
			"transport failure classification changed");
	}
}

void ftp_and_sftp_use_temporary_remote_names()
{
	FakeTransfer transfer;
	RuntimeDataChannelConfiguration ftp;
	ftp.id = "channel-ftp";
	ftp.protocol = OutboundProtocol::Ftp;
	ftp.host = "ftp.example.test";
	ftp.remote_directory = "/incoming/meter";
	ftp.authentication = RuntimeAuthentication::Password;
	ftp.username = "meter";
	ftp.password = "secret";
	ConfiguredDataChannel ftp_channel(ftp, transfer);
	const auto generated = content();
	const auto ftp_result = ftp_channel.deliver({ftp.id, generated, false});
	require(ftp_result.disposition == DeliveryDisposition::Succeeded &&
		transfer.last_plan->remote_temporary_path ==
			"/incoming/meter/.job-r1-0-300-json.json.part" &&
		transfer.last_plan->remote_final_path ==
			"/incoming/meter/job-r1-0-300-json.json",
		"FTP did not plan a temporary upload followed by final rename");

	RuntimeDataChannelConfiguration sftp;
	sftp.id = "channel-sftp";
	sftp.protocol = OutboundProtocol::Sftp;
	sftp.host = "sftp.example.test";
	sftp.remote_directory = "/drop";
	sftp.authentication = RuntimeAuthentication::PrivateKey;
	sftp.username = "meter";
	sftp.sftp_private_key_file = "/run/data-sender/channel/key.pem";
	sftp.known_hosts_file = "/run/data-sender/channel/known-hosts.txt";
	ConfiguredDataChannel sftp_channel(sftp, transfer);
	(void)sftp_channel.deliver({sftp.id, generated, false});
	require(transfer.last_plan->protocol == OutboundProtocol::Sftp &&
		transfer.last_plan->known_hosts_file == sftp.known_hosts_file &&
		transfer.last_plan->sftp_private_key_file ==
			sftp.sftp_private_key_file,
		"SFTP plan did not retain pinned host/key material");
}

void secure_channel_assets_are_mandatory()
{
	FakeTransfer transfer;
	RuntimeDataChannelConfiguration sftp;
	sftp.id = "invalid-sftp";
	sftp.protocol = OutboundProtocol::Sftp;
	sftp.host = "sftp.example.test";
	sftp.authentication = RuntimeAuthentication::Password;
	sftp.username = "meter";
	bool rejected = false;
	try {
		ConfiguredDataChannel channel(sftp, transfer);
		(void)channel;
	} catch (const std::invalid_argument &) {
		rejected = true;
	}
	require(rejected, "SFTP accepted an unverified host key");

	RuntimeDataChannelConfiguration https;
	https.id = "invalid-https";
	https.protocol = OutboundProtocol::Https;
	https.host = "collector.example.test";
	https.use_system_ca = false;
	rejected = false;
	try {
		ConfiguredDataChannel channel(https, transfer);
		(void)channel;
	} catch (const std::invalid_argument &) {
		rejected = true;
	}
	require(rejected, "HTTPS accepted no trusted CA source");
}

} // namespace

int main()
{
	http_plan_carries_identity_and_never_embeds_secrets();
	http_result_classification_is_stable();
	ftp_and_sftp_use_temporary_remote_names();
	secure_channel_assets_are_mandatory();
}
