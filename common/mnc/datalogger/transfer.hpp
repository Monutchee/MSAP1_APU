#pragma once

#include "mnc/datalogger/data_channel.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mnc::datalogger {

enum class OutboundProtocol : std::uint8_t { Http, Https, Ftp, Sftp };
enum class RuntimeAuthentication : std::uint8_t {
	None,
	Basic,
	Bearer,
	MutualTls,
	Password,
	PrivateKey,
};

struct RuntimeDataChannelConfiguration {
	std::string id;
	OutboundProtocol protocol = OutboundProtocol::Https;
	std::string host;
	std::uint16_t port = 0;
	std::string http_path = "/";
	std::string remote_directory = "/";
	RuntimeAuthentication authentication = RuntimeAuthentication::None;
	std::string username;
	std::string password;
	std::string bearer_token;
	std::string private_key_passphrase;
	std::uint32_t connect_timeout_seconds = 10;
	std::uint32_t transfer_timeout_seconds = 120;
	bool use_system_ca = true;
	std::optional<std::filesystem::path> ca_file;
	std::optional<std::filesystem::path> client_certificate_file;
	std::optional<std::filesystem::path> client_key_file;
	std::optional<std::filesystem::path> sftp_private_key_file;
	std::optional<std::filesystem::path> known_hosts_file;
};

struct TransferPlan {
	OutboundProtocol protocol = OutboundProtocol::Https;
	std::string url;
	std::string filename;
	std::string artifact_id;
	std::string checksum_sha256;
	std::string mime_type;
	std::string body;
	std::vector<std::string> headers;
	std::optional<std::string> remote_temporary_path;
	std::optional<std::string> remote_final_path;
	RuntimeAuthentication authentication = RuntimeAuthentication::None;
	std::string username;
	std::string password;
	std::string bearer_token;
	std::string private_key_passphrase;
	std::uint32_t connect_timeout_seconds = 10;
	std::uint32_t transfer_timeout_seconds = 120;
	bool use_system_ca = true;
	std::optional<std::filesystem::path> ca_file;
	std::optional<std::filesystem::path> client_certificate_file;
	std::optional<std::filesystem::path> client_key_file;
	std::optional<std::filesystem::path> sftp_private_key_file;
	std::optional<std::filesystem::path> known_hosts_file;
	bool zero_data_probe = false;
};

enum class TransferFailure : std::uint8_t {
	None,
	Transient,
	Authentication,
	Verification,
	Configuration,
	RemoteRejected,
};

struct TransferOutcome {
	bool transport_succeeded = false;
	long response_code = 0;
	TransferFailure failure = TransferFailure::Transient;
	std::string sanitized_message;
};

class TransferClient {
public:
	virtual ~TransferClient() = default;
	[[nodiscard]] virtual TransferOutcome perform(const TransferPlan &plan) = 0;
};

class ConfiguredDataChannel final : public DataChannel {
public:
	ConfiguredDataChannel(RuntimeDataChannelConfiguration configuration,
		TransferClient &transfer);
	[[nodiscard]] std::string_view protocol() const noexcept override;
	[[nodiscard]] DeliveryResult deliver(
		const DeliveryRequest &request) override;

private:
	RuntimeDataChannelConfiguration configuration_;
	TransferClient &transfer_;
};

/** libcurl adapter. A host build without development headers returns blocked. */
class CurlTransferClient final : public TransferClient {
public:
	CurlTransferClient();
	~CurlTransferClient() override;
	[[nodiscard]] TransferOutcome perform(const TransferPlan &plan) override;

private:
	class Implementation;
	std::unique_ptr<Implementation> implementation_;
};

[[nodiscard]] DeliveryResult classify_transfer_result(
	OutboundProtocol protocol, const TransferOutcome &outcome);
[[nodiscard]] bool curl_transfer_available() noexcept;

} // namespace mnc::datalogger
