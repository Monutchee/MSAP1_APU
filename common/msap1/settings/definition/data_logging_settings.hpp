#pragma once

#include <glaze/glaze.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace msap1::settings {

/** Stable outbound protocol identities persisted in schema-v5 settings. */
enum class DataChannelProtocol : std::uint8_t { http, https, ftp, sftp };

/**
 * Authentication choices are deliberately protocol-neutral on the wire.
 * DataLoggingSettings::validate() constrains each choice to protocols that
 * can implement it safely.
 */
enum class DataChannelAuthentication : std::uint8_t {
	none,
	basic,
	bearer,
	mtls,
	password,
	private_key,
};

struct DataChannelSettings {
	std::string id;
	std::string name;
	bool enabled = true;
	DataChannelProtocol protocol = DataChannelProtocol::https;
	std::string host;
	/** Zero selects the protocol default (80, 443, 21, or 22). */
	std::uint16_t port = 0;
	/** HTTP request path. Ignored by FTP and SFTP. */
	std::string http_path = "/";
	/** FTP/SFTP destination directory. Ignored by HTTP(S). */
	std::string remote_directory = "/";
	DataChannelAuthentication authentication =
		DataChannelAuthentication::none;
	std::string username;
	std::uint32_t connect_timeout_seconds = 10;
	std::uint32_t transfer_timeout_seconds = 120;
	/** HTTPS uses the packaged system CA unless an uploaded channel CA is chosen. */
	bool use_system_ca = true;
	bool use_uploaded_ca = false;
	bool use_client_certificate = false;
	/** Required opt-in for clear-text HTTP and FTP. */
	bool insecure_transport_acknowledged = false;

	bool operator==(const DataChannelSettings &) const = default;
};

struct DataLoggingSelectionSettings {
	/** Canonical MeterAttributeDescriptor::key. */
	std::string attribute;
	/** minimum, maximum, average, last, circular_average, first, or delta. */
	std::string calculation = "last";

	bool operator==(const DataLoggingSelectionSettings &) const = default;
};

enum class DataLoggingDestination : std::uint8_t { remote, local_only };

struct DataLoggingJobSettings {
	std::string id;
	std::string name;
	bool enabled = false;
	/** Incremented whenever generation-relevant configuration changes. */
	std::uint64_t revision = 1;
	/** Canonical historian period key. */
	std::string source_period = "basic";
	std::uint64_t generation_interval_seconds = 300;
	std::uint64_t row_interval_seconds = 60;
	std::vector<DataLoggingSelectionSettings> selections;
	/** json or csv. */
	std::string format = "json";
	DataLoggingDestination destination = DataLoggingDestination::remote;
	std::vector<std::string> channel_ids;

	bool operator==(const DataLoggingJobSettings &) const = default;
};

struct DataLoggingStorageSettings {
	std::uint64_t maximum_bytes = 512ull * 1024ull * 1024ull;
	std::uint64_t minimum_free_bytes = 256ull * 1024ull * 1024ull;
	std::uint32_t completed_metadata_retention_days = 30;

	bool operator==(const DataLoggingStorageSettings &) const = default;
};

/**
 * Public, non-secret M19 configuration. An empty jobs vector is the disabled
 * state; queued work is intentionally not governed by a top-level switch.
 */
struct DataLoggingSettings {
	std::vector<DataChannelSettings> channels;
	std::vector<DataLoggingJobSettings> jobs;
	DataLoggingStorageSettings storage;

	void validate(std::uint32_t demand_window_seconds) const;
	bool operator==(const DataLoggingSettings &) const = default;
};

[[nodiscard]] bool valid_data_channel_id(std::string_view value) noexcept;

} // namespace msap1::settings

template<>
struct glz::meta<msap1::settings::DataChannelProtocol> {
	static constexpr auto value = glz::enumerate(
		"http", msap1::settings::DataChannelProtocol::http,
		"https", msap1::settings::DataChannelProtocol::https,
		"ftp", msap1::settings::DataChannelProtocol::ftp,
		"sftp", msap1::settings::DataChannelProtocol::sftp);
};

template<>
struct glz::meta<msap1::settings::DataChannelAuthentication> {
	static constexpr auto value = glz::enumerate(
		"none", msap1::settings::DataChannelAuthentication::none,
		"basic", msap1::settings::DataChannelAuthentication::basic,
		"bearer", msap1::settings::DataChannelAuthentication::bearer,
		"mtls", msap1::settings::DataChannelAuthentication::mtls,
		"password", msap1::settings::DataChannelAuthentication::password,
		"private_key",
		msap1::settings::DataChannelAuthentication::private_key);
};

template<>
struct glz::meta<msap1::settings::DataLoggingDestination> {
	static constexpr auto value = glz::enumerate(
		"remote", msap1::settings::DataLoggingDestination::remote,
		"local_only", msap1::settings::DataLoggingDestination::local_only);
};
