#pragma once

#include "msap1/datalogger/data_sender_ipc.hpp"
#include "msap1/settings/settings_ipc.hpp"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace msap1::web {

class DataSenderGatewayError final : public std::runtime_error {
public:
	DataSenderGatewayError(datalogger::ipc::Status status, std::string message);
	[[nodiscard]] datalogger::ipc::Status status() const noexcept;

private:
	datalogger::ipc::Status status_;
};

struct DataChannelMaterialStatus {
	std::string channel_id;
	bool password_configured = false;
	bool bearer_token_configured = false;
	bool private_key_passphrase_configured = false;
	bool ca_configured = false;
	bool client_certificate_configured = false;
	bool client_key_configured = false;
	bool sftp_private_key_configured = false;
	bool known_hosts_configured = false;
};

/** Typed Web boundary for Data Sender runtime and channel material status. */
class DataSenderGateway final {
public:
	using ServiceStatus = datalogger::ipc::ServiceStatus;
	using ArtifactList = datalogger::ipc::ArtifactList;
	using ArtifactDetail = datalogger::ipc::ArtifactDetail;
	using DeletionResult = datalogger::ipc::DeletionResult;
	using ChannelTestResult = datalogger::ipc::ChannelTestResult;
	using Chunk = datalogger::ipc::Response;

	[[nodiscard]] ServiceStatus status(int timeout_ms = 5000) const;
	[[nodiscard]] ArtifactList artifacts(std::uint64_t offset,
		std::uint32_t limit, std::string job_id = {}, std::string state = {},
		std::optional<std::int64_t> start = {},
		std::optional<std::int64_t> end = {}, int timeout_ms = 5000) const;
	[[nodiscard]] ArtifactDetail artifact(std::string id,
		int timeout_ms = 5000) const;
	[[nodiscard]] std::string preview(std::string id, std::uint32_t limit,
		int timeout_ms = 5000) const;
	[[nodiscard]] Chunk read_chunk(std::string id, std::uint64_t offset,
		std::uint32_t limit, int timeout_ms = 5000) const;
	void retry(std::vector<std::string> ids, int timeout_ms = 5000) const;
	[[nodiscard]] DeletionResult erase(std::vector<std::string> ids,
		bool discard_unsent, int timeout_ms = 5000) const;
	[[nodiscard]] ChannelTestResult test_channel(std::string id,
		int timeout_ms = 150000) const;

	[[nodiscard]] DataChannelMaterialStatus materials(std::string_view channel_id,
		int timeout_ms = 3000) const;
	void set_secret(std::string_view channel_id, std::string_view kind,
		std::string value, int timeout_ms = 5000) const;
	void clear_secret(std::string_view channel_id, std::string_view kind,
		int timeout_ms = 5000) const;
	void upload_asset(std::string_view channel_id, std::string_view kind,
		std::string contents, int timeout_ms = 5000) const;
	void delete_asset(std::string_view channel_id, std::string_view kind,
		int timeout_ms = 5000) const;

private:
	[[nodiscard]] datalogger::ipc::Response require_ok(
		datalogger::ipc::Request request, int timeout_ms) const;
	[[nodiscard]] static std::string scoped_name(std::string_view channel_id,
		std::string_view kind, bool asset);

	mutable datalogger::ipc::DataSenderClient sender_;
	mutable settings::ipc::SettingsClient settings_;
};

} // namespace msap1::web
