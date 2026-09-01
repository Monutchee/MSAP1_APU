#include "data_sender_service.hpp"
#include "ipc_access_policy.hpp"

#include "mnc/settings/settings.hpp"
#include "msap1/settings/settings_ipc.hpp"

#include <glaze/glaze.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <ranges>
#include <stdexcept>
#include <utility>

#include <sys/stat.h>

namespace msap1::datalogger::daemon {
namespace {

using namespace std::chrono_literals;
using mnc::datalogger::ArtifactState;
using mnc::datalogger::ContentFormat;
using mnc::datalogger::DeliveryState;
using mnc::datalogger::OutboundProtocol;
using mnc::datalogger::RuntimeAuthentication;
using msap1::settings::DataChannelAuthentication;
using msap1::settings::DataChannelProtocol;

constexpr std::string_view persistent_root = "/data/mnc/data-sender";

std::string encode_json(const auto &value)
{
	const auto encoded = glz::write_json(value);
	if (!encoded)
		throw std::runtime_error("cannot encode Data Sender IPC JSON");
	return *encoded;
}

OutboundProtocol protocol(DataChannelProtocol value)
{
	switch (value) {
	case DataChannelProtocol::http: return OutboundProtocol::Http;
	case DataChannelProtocol::https: return OutboundProtocol::Https;
	case DataChannelProtocol::ftp: return OutboundProtocol::Ftp;
	case DataChannelProtocol::sftp: return OutboundProtocol::Sftp;
	}
	throw std::invalid_argument("unknown data channel protocol");
}

std::string protocol_name(DataChannelProtocol value)
{
	switch (value) {
	case DataChannelProtocol::http: return "http";
	case DataChannelProtocol::https: return "https";
	case DataChannelProtocol::ftp: return "ftp";
	case DataChannelProtocol::sftp: return "sftp";
	}
	return "unknown";
}

RuntimeAuthentication authentication(DataChannelAuthentication value)
{
	switch (value) {
	case DataChannelAuthentication::none: return RuntimeAuthentication::None;
	case DataChannelAuthentication::basic: return RuntimeAuthentication::Basic;
	case DataChannelAuthentication::bearer: return RuntimeAuthentication::Bearer;
	case DataChannelAuthentication::mtls:
		return RuntimeAuthentication::MutualTls;
	case DataChannelAuthentication::password:
		return RuntimeAuthentication::Password;
	case DataChannelAuthentication::private_key:
		return RuntimeAuthentication::PrivateKey;
	}
	throw std::invalid_argument("unknown data channel authentication");
}

mnc::meter::MeasurementPeriod period(std::string_view key)
{
	for (const auto &candidate : mnc::meter::defined_measurement_periods())
		if (candidate.key == key && candidate.historian)
			return candidate.period;
	throw std::invalid_argument("unknown Data Sender source period");
}

mnc::meter::MeterAttributeCalculation calculation(std::string_view key)
{
	using Value = mnc::meter::MeterAttributeCalculation;
	if (key == "minimum") return Value::Minimum;
	if (key == "maximum") return Value::Maximum;
	if (key == "average") return Value::Average;
	if (key == "last") return Value::Last;
	if (key == "circular_average") return Value::CircularAverage;
	if (key == "first") return Value::First;
	if (key == "delta") return Value::Delta;
	throw std::invalid_argument("unknown Data Sender calculation");
}

ContentFormat format(std::string_view key)
{
	if (key == "json") return ContentFormat::Json;
	if (key == "csv") return ContentFormat::Csv;
	throw std::invalid_argument("unknown Data Sender format");
}

std::string artifact_state(ArtifactState value)
{
	switch (value) {
	case ArtifactState::Pending: return "pending";
	case ArtifactState::PartiallyDelivered: return "partially_delivered";
	case ArtifactState::Blocked: return "blocked";
	case ArtifactState::Succeeded: return "succeeded";
	case ArtifactState::LocalOnly: return "local_only";
	case ArtifactState::MissingPayload: return "missing_payload";
	}
	return "unknown";
}

std::optional<ArtifactState> artifact_state(std::string_view value)
{
	if (value.empty()) return std::nullopt;
	if (value == "pending") return ArtifactState::Pending;
	if (value == "partially_delivered")
		return ArtifactState::PartiallyDelivered;
	if (value == "blocked") return ArtifactState::Blocked;
	if (value == "succeeded") return ArtifactState::Succeeded;
	if (value == "local_only") return ArtifactState::LocalOnly;
	if (value == "missing_payload") return ArtifactState::MissingPayload;
	throw std::invalid_argument("unknown artifact state filter");
}

std::string delivery_state(DeliveryState value)
{
	switch (value) {
	case DeliveryState::Pending: return "pending";
	case DeliveryState::RetryWait: return "retry_wait";
	case DeliveryState::Blocked: return "blocked";
	case DeliveryState::Succeeded: return "succeeded";
	case DeliveryState::AdministrativelyDiscarded:
		return "administratively_discarded";
	case DeliveryState::InFlight: return "in_flight";
	}
	return "unknown";
}

ipc::ArtifactSummary artifact_dto(
	const mnc::datalogger::ArtifactSummary &source)
{
	return {
		.id = source.artifact_id,
		.job_id = source.job_id,
		.job_revision = source.job_revision,
		.filename = source.filename,
		.mime_type = source.mime_type,
		.sha256 = source.sha256,
		.size_bytes = source.size_bytes,
		.source_start_nanoseconds = source.source_window.start,
		.source_end_nanoseconds = source.source_window.end,
		.generated_at_nanoseconds = source.generated_at,
		.created_at_nanoseconds = source.created_at,
		.state = artifact_state(source.state),
		.local_only = source.local_only,
		.payload_present = source.payload_present,
		.delivery_count = source.delivery_count,
		.succeeded_count = source.succeeded_count,
		.blocked_count = source.blocked_count,
		.recovery_error = source.recovery_error,
	};
}

std::string required_value(const std::map<std::string, std::string> &values,
	std::string_view key, std::string_view description)
{
	const auto found = values.find(std::string(key));
	if (found == values.end() || found->second.empty())
		throw std::runtime_error(std::string(description) + " is not installed");
	return found->second;
}

} // namespace

struct DataSenderService::ChannelRuntime {
	msap1::settings::DataChannelSettings settings;
	ipc::ChannelStatus status;
	std::unique_ptr<mnc::datalogger::ConfiguredDataChannel> channel;
	mutable std::mutex mutex;
};

DataSenderService::DataSenderService()
	: Service("MSAP1 Data Sender", "data-sender"), historian_(),
	  datalogger_(historian_), outbox_(std::filesystem::path(persistent_root)),
	  engine_(datalogger_, writers_, outbox_, clock_),
	  server_(context_.get_executor(), std::string(ipc::socket_path))
{
}

DataSenderService::ConfigurationBundle
DataSenderService::load_configuration() const
{
	ConfigurationBundle result;
	const msap1::settings::ipc::SettingsClient client;
	result.settings = client.active();
	result.settings.data_logging.validate(
		result.settings.metering.demand.window_seconds);
	for (const auto &channel : result.settings.data_logging.channels) {
		result.credentials.emplace(channel.id,
			client.runtime_data_channel_credentials(channel.id));
		result.assets.emplace(channel.id,
			client.runtime_data_channel_assets(channel.id));
	}
	return result;
}

void DataSenderService::materialize_assets(std::string_view channel_id,
	const std::map<std::string, std::string> &assets)
{
	const auto directory = runtime_assets_root_ / std::string(channel_id);
	std::filesystem::create_directories(directory);
	std::filesystem::permissions(directory,
		std::filesystem::perms::owner_all,
		std::filesystem::perm_options::replace);
	const std::array files{
		std::pair{std::string_view{"ca"}, std::string_view{"ca.pem"}},
		std::pair{std::string_view{"client-certificate"},
			std::string_view{"client-certificate.pem"}},
		std::pair{std::string_view{"client-key"},
			std::string_view{"client-key.pem"}},
		std::pair{std::string_view{"sftp-private-key"},
			std::string_view{"sftp-private-key.pem"}},
		std::pair{std::string_view{"known-hosts"},
			std::string_view{"known-hosts.txt"}},
	};
	for (const auto &[key, filename] : files) {
		const auto found = assets.find(std::string(key));
		const auto path = directory / filename;
		if (found != assets.end())
			mnc::settings::AtomicFileWriter::write(path, found->second, 0600);
		else {
			std::error_code ignored;
			std::filesystem::remove(path, ignored);
		}
	}
}

void DataSenderService::cleanup_assets(std::string_view channel_id) noexcept
{
	const auto directory = runtime_assets_root_ / std::string(channel_id);
	for (const auto filename : {"ca.pem", "client-certificate.pem",
		     "client-key.pem", "sftp-private-key.pem", "known-hosts.txt"}) {
		std::error_code ignored;
		std::filesystem::remove(directory / filename, ignored);
	}
	std::error_code ignored;
	std::filesystem::remove(directory, ignored);
}

void DataSenderService::apply_configuration(ConfigurationBundle bundle)
{
	const auto &logging = bundle.settings.data_logging;
	outbox_.update_storage_policy({logging.storage.maximum_bytes,
		logging.storage.minimum_free_bytes,
		static_cast<mnc::datalogger::UtcNanoseconds>(
			logging.storage.completed_metadata_retention_days) *
			24ll * 60ll * 60ll * 1'000'000'000ll});
	outbox_.prune_completed(clock_.now());
	std::unordered_map<std::string, std::shared_ptr<ChannelRuntime>> next_channels;
	for (const auto &channel_settings : logging.channels) {
		auto runtime = std::make_shared<ChannelRuntime>();
		runtime->settings = channel_settings;
		runtime->status = {
			.id = channel_settings.id,
			.name = channel_settings.name,
			.protocol = protocol_name(channel_settings.protocol),
			.enabled = channel_settings.enabled,
			.ready = false,
			.readiness_error = {},
			.last_test_state = "never",
			.last_test_message = {},
			.last_test_at_nanoseconds = 0,
		};
		{
			std::scoped_lock lock(configuration_mutex_);
			const auto previous = channels_.find(channel_settings.id);
			if (previous != channels_.end()) {
				std::scoped_lock channel_lock(previous->second->mutex);
				runtime->status.last_test_state =
					previous->second->status.last_test_state;
				runtime->status.last_test_message =
					previous->second->status.last_test_message;
				runtime->status.last_test_at_nanoseconds =
					previous->second->status.last_test_at_nanoseconds;
			}
		}
		try {
			const auto &credentials = bundle.credentials.at(channel_settings.id);
			const auto &assets = bundle.assets.at(channel_settings.id);
			materialize_assets(channel_settings.id, assets);
			mnc::datalogger::RuntimeDataChannelConfiguration configuration;
			configuration.id = channel_settings.id;
			configuration.protocol = protocol(channel_settings.protocol);
			configuration.host = channel_settings.host;
			configuration.port = channel_settings.port;
			configuration.http_path = channel_settings.http_path;
			configuration.remote_directory = channel_settings.remote_directory;
			configuration.authentication = authentication(
				channel_settings.authentication);
			configuration.username = channel_settings.username;
			configuration.connect_timeout_seconds =
				channel_settings.connect_timeout_seconds;
			configuration.transfer_timeout_seconds =
				channel_settings.transfer_timeout_seconds;
			configuration.use_system_ca = channel_settings.use_system_ca;
			if (channel_settings.authentication ==
			    DataChannelAuthentication::basic ||
			    channel_settings.authentication ==
				DataChannelAuthentication::password)
				configuration.password = required_value(credentials, "password",
					"channel password");
			if (channel_settings.authentication ==
			    DataChannelAuthentication::bearer)
				configuration.bearer_token = required_value(credentials,
					"bearer-token", "channel bearer token");
			if (const auto passphrase = credentials.find(
				"private-key-passphrase"); passphrase != credentials.end())
				configuration.private_key_passphrase = passphrase->second;
			const auto asset_root = runtime_assets_root_ / channel_settings.id;
			if (channel_settings.use_uploaded_ca) {
				(void)required_value(assets, "ca", "channel CA");
				configuration.ca_file = asset_root / "ca.pem";
			}
			if (channel_settings.use_client_certificate) {
				(void)required_value(assets, "client-certificate",
					"channel client certificate");
				(void)required_value(assets, "client-key",
					"channel client key");
				configuration.client_certificate_file =
					asset_root / "client-certificate.pem";
				configuration.client_key_file = asset_root / "client-key.pem";
			}
			if (channel_settings.protocol == DataChannelProtocol::sftp) {
				(void)required_value(assets, "known-hosts",
					"SFTP known-host key");
				configuration.known_hosts_file =
					asset_root / "known-hosts.txt";
			}
			if (channel_settings.authentication ==
			    DataChannelAuthentication::private_key) {
				(void)required_value(assets, "sftp-private-key",
					"SFTP private key");
				configuration.sftp_private_key_file =
					asset_root / "sftp-private-key.pem";
			}
			if (!mnc::datalogger::curl_transfer_available())
				throw std::runtime_error(
					"libcurl transport support is unavailable");
			runtime->channel =
				std::make_unique<mnc::datalogger::ConfiguredDataChannel>(
					std::move(configuration), transfer_);
			runtime->status.ready = true;
		} catch (const std::exception &error) {
			runtime->status.readiness_error = error.what();
		}
		next_channels.emplace(channel_settings.id, std::move(runtime));
	}

	std::vector<mnc::datalogger::ScheduledJob> jobs;
	jobs.reserve(logging.jobs.size());
	const auto device_id = bundle.settings.waveform.device_serial.empty()
		? std::string{"msap1"} : bundle.settings.waveform.device_serial;
	for (const auto &configured : logging.jobs) {
		mnc::datalogger::ScheduledJob scheduled;
		scheduled.enabled = configured.enabled;
		scheduled.local_only = configured.destination ==
			msap1::settings::DataLoggingDestination::local_only;
		scheduled.channel_ids = configured.channel_ids;
		auto &snapshot = scheduled.snapshot;
		snapshot.job_id = configured.id;
		snapshot.revision = configured.revision;
		snapshot.product_id = "msap1";
		snapshot.device_id = device_id;
		snapshot.source_period = period(configured.source_period);
		snapshot.generation_interval_nanoseconds =
			static_cast<std::int64_t>(configured.generation_interval_seconds) *
			1'000'000'000ll;
		snapshot.row_interval_nanoseconds =
			static_cast<std::int64_t>(configured.row_interval_seconds) *
			1'000'000'000ll;
		snapshot.format = format(configured.format);
		for (const auto &selection : configured.selections)
			snapshot.selections.push_back({
				.attribute = *mnc::meter::find_attribute(selection.attribute),
				.calculation = calculation(selection.calculation),
			});
		jobs.push_back(std::move(scheduled));
	}
	engine_.apply_jobs(std::move(jobs));
	const auto now = clock_.now();
	for (const auto &[id, runtime] : next_channels)
		if (runtime->status.ready)
			outbox_.retry_channel(id, now);
	{
		std::scoped_lock lock(configuration_mutex_);
		for (const auto &[id, runtime] : channels_)
			if (!next_channels.contains(id))
				cleanup_assets(id);
		channels_ = std::move(next_channels);
		active_bundle_ = std::move(bundle);
		pending_bundle_.reset();
	}
	wait_condition_.notify_all();
}

void DataSenderService::on_start()
{
	outbox_.initialize();
	apply_configuration(load_configuration());
	server_.start(
		[this](auto connection, auto frame) {
			handle(std::move(connection), std::move(frame));
		},
		[this](const std::string &error) {
			(void)logger().write(mnc::logging::Priority::warning,
				"Data Sender IPC error: " + error, "ipc_error");
		});
	if (::chmod(ipc::socket_path.data(), 0660) != 0)
		throw std::runtime_error("cannot set Data Sender socket mode");
	io_worker_ = std::thread([this] {
		try {
			context_.run();
		} catch (const std::exception &error) {
			{
				std::scoped_lock lock(failure_mutex_);
				failure_message_ = error.what();
			}
			worker_failed_ = true;
			request_stop();
		}
	});
	scheduler_worker_ = std::thread([this] { scheduler_loop(); });
	for (int index = 0; index < 2; ++index)
		delivery_workers_.emplace_back([this] { delivery_loop(); });
	settings_worker_ = std::thread([this] { settings_loop(); });
}

void DataSenderService::scheduler_loop()
{
	try {
		while (!stopping_) {
			(void)engine_.generate_due();
			std::unique_lock lock(wait_mutex_);
			wait_condition_.wait_for(lock, 500ms,
				[this] { return stopping_.load(); });
		}
	} catch (const std::exception &error) {
		{
			std::scoped_lock lock(failure_mutex_);
			failure_message_ = error.what();
		}
		worker_failed_ = true;
		request_stop();
	}
}

mnc::datalogger::DeliveryResult DataSenderService::deliver(
	std::string_view channel_id,
	const mnc::datalogger::DeliveryRequest &request)
{
	std::shared_ptr<ChannelRuntime> runtime;
	{
		std::scoped_lock lock(configuration_mutex_);
		const auto found = channels_.find(std::string(channel_id));
		if (found == channels_.end())
			return {mnc::datalogger::DeliveryDisposition::Blocked, {},
				"selected data channel no longer exists"};
		runtime = found->second;
	}
	std::scoped_lock lock(runtime->mutex);
	if (!runtime->settings.enabled)
		return {mnc::datalogger::DeliveryDisposition::Blocked, {},
			"selected data channel is disabled"};
	if (!runtime->channel)
		return {mnc::datalogger::DeliveryDisposition::Blocked, {},
			runtime->status.readiness_error};
	return runtime->channel->deliver(request);
}

void DataSenderService::delivery_loop()
{
	try {
		while (!stopping_) {
			const auto count = engine_.deliver_due(
				[this](auto id, const auto &request) {
					return deliver(id, request);
				}, 1);
			if (count == 0) {
				std::unique_lock lock(wait_mutex_);
				wait_condition_.wait_for(lock, 250ms,
					[this] { return stopping_.load(); });
			}
		}
	} catch (const std::exception &error) {
		{
			std::scoped_lock lock(failure_mutex_);
			failure_message_ = error.what();
		}
		worker_failed_ = true;
		request_stop();
	}
}

void DataSenderService::settings_loop()
{
	while (!stopping_) {
		std::unique_lock wait_lock(wait_mutex_);
		wait_condition_.wait_for(wait_lock, 2s,
			[this] { return stopping_.load(); });
		wait_lock.unlock();
		if (stopping_)
			break;
		try {
			auto candidate = load_configuration();
			bool changed = false;
			{
				std::scoped_lock lock(configuration_mutex_);
				changed = candidate != active_bundle_;
				if (changed)
					pending_bundle_ = std::move(candidate);
			}
			if (changed)
				request_reload();
		} catch (const std::exception &error) {
			(void)logger().write(mnc::logging::Priority::debug,
				"Data Sender settings check deferred: " +
					std::string(error.what()),
				"settings_check_deferred");
		}
	}
}

void DataSenderService::on_reload()
{
	try {
		std::optional<ConfigurationBundle> pending;
		{
			std::scoped_lock lock(configuration_mutex_);
			if (pending_bundle_)
				pending = *pending_bundle_;
		}
		apply_configuration(pending ? std::move(*pending) :
			load_configuration());
		(void)logger().write(mnc::logging::Priority::notice,
			"Data Sender configuration applied", "configuration_applied");
	} catch (const std::exception &error) {
		(void)logger().write(mnc::logging::Priority::warning,
			"Data Sender retained its previous configuration: " +
				std::string(error.what()),
			"configuration_reload_failed");
	}
}

void DataSenderService::on_stop() noexcept
{
	stopping_ = true;
	wait_condition_.notify_all();
	if (settings_worker_.joinable()) settings_worker_.join();
	if (scheduler_worker_.joinable()) scheduler_worker_.join();
	for (auto &worker : delivery_workers_)
		if (worker.joinable()) worker.join();
	server_.stop();
	context_.stop();
	if (io_worker_.joinable()) io_worker_.join();
	std::vector<std::string> channel_ids;
	{
		std::scoped_lock lock(configuration_mutex_);
		for (const auto &[id, runtime] : channels_) channel_ids.push_back(id);
	}
	for (const auto &id : channel_ids) cleanup_assets(id);
}

ipc::ServiceStatus DataSenderService::status_snapshot() const
{
	const auto outbox = outbox_.status();
	ipc::ServiceStatus result;
	result.health = (!outbox.storage.generation_allowed ||
		outbox.missing_payload_count != 0) ? "critical" :
		(outbox.blocked_delivery_count != 0 ? "warning" : "ready");
	if (!outbox.storage.generation_allowed)
		result.message = outbox.storage.blocking_reason;
	else if (outbox.missing_payload_count != 0)
		result.message =
			"one or more generated payloads are missing or damaged";
	else if (outbox.blocked_delivery_count != 0)
		result.message =
			"one or more deliveries need configuration attention";
	else
		result.message = "Data Sender ready";
	result.artifact_count = outbox.artifact_count;
	result.outbox_count = outbox.outbox_count;
	result.outbox_bytes = outbox.outbox_bytes;
	result.archive_count = outbox.archive_count;
	result.archive_bytes = outbox.archive_bytes;
	result.completed_metadata_count = outbox.completed_metadata_count;
	result.missing_payload_count = outbox.missing_payload_count;
	result.pending_delivery_count = outbox.pending_delivery_count;
	result.blocked_delivery_count = outbox.blocked_delivery_count;
	result.oldest_pending_created_at_nanoseconds =
		outbox.oldest_pending_created_at;
	result.maximum_bytes = outbox.storage.maximum_bytes;
	result.available_bytes = outbox.storage.available_bytes;
	result.minimum_free_bytes = outbox.storage.minimum_free_bytes;
	result.generation_allowed = outbox.storage.generation_allowed;
	result.storage_blocking_reason = outbox.storage.blocking_reason;
	for (const auto &status : engine_.job_status()) {
		ipc::JobStatus job{.id = status.job_id, .revision = status.revision,
			.enabled = status.enabled,
			.next_start_nanoseconds = std::nullopt,
			.next_end_nanoseconds = std::nullopt,
			.last_start_nanoseconds = std::nullopt,
			.last_end_nanoseconds = std::nullopt,
			.last_generated_at_nanoseconds = status.last_generated_at,
			.last_error = status.last_error};
		if (status.next_window) {
			job.next_start_nanoseconds = status.next_window->start;
			job.next_end_nanoseconds = status.next_window->end;
		}
		if (status.last_generated_window) {
			job.last_start_nanoseconds = status.last_generated_window->start;
			job.last_end_nanoseconds = status.last_generated_window->end;
		}
		result.jobs.push_back(std::move(job));
	}
	std::scoped_lock lock(configuration_mutex_);
	for (const auto &[id, runtime] : channels_) {
		std::scoped_lock channel_lock(runtime->mutex);
		result.channels.push_back(runtime->status);
	}
	std::ranges::sort(result.channels, {}, &ipc::ChannelStatus::id);
	return result;
}

ipc::ChannelTestResult DataSenderService::test_channel(std::string_view id)
{
	std::shared_ptr<ChannelRuntime> runtime;
	{
		std::scoped_lock lock(configuration_mutex_);
		const auto found = channels_.find(std::string(id));
		if (found == channels_.end())
			throw std::out_of_range("unknown data channel ID");
		runtime = found->second;
	}
	const auto now = clock_.now();
	mnc::datalogger::GeneratedDataset dataset;
	dataset.artifact_id = "channel-test-" + std::to_string(now);
	dataset.job_id = "channel-test";
	dataset.job_revision = 1;
	dataset.product_id = "msap1";
	dataset.device_id = "zero-data-probe";
	dataset.format = ContentFormat::Json;
	dataset.generated_at = now;
	dataset.artifact_window = {now, now + 1};
	const auto generated = mnc::datalogger::JsonMeterDataContentWriter{}.
		write(dataset);
	mnc::datalogger::DeliveryResult delivery_result;
	std::string state;
	std::string message;
	{
		std::scoped_lock lock(runtime->mutex);
		if (!runtime->channel)
			delivery_result = {
				mnc::datalogger::DeliveryDisposition::Blocked, {},
				runtime->status.readiness_error};
		else
			delivery_result = runtime->channel->deliver(
				{std::string(id), generated, true});
		runtime->status.last_test_at_nanoseconds = now;
		runtime->status.last_test_state = delivery_result.disposition ==
			mnc::datalogger::DeliveryDisposition::Succeeded ? "succeeded" :
			(delivery_result.disposition ==
				mnc::datalogger::DeliveryDisposition::Retryable
					? "retryable_failure" : "blocked");
		runtime->status.last_test_message = delivery_result.sanitized_error;
		state = runtime->status.last_test_state;
		message = runtime->status.last_test_message;
	}
	return {std::string(id), std::move(state), std::move(message), now};
}

void DataSenderService::handle(
	mnc::ipc::UnixStreamServer::Connection connection, mnc::ipc::Frame frame)
{
	ipc::Response response;
	ipc::Command command = ipc::Command::get_status;
	try {
		const auto request = ipc::decode_request(frame);
		command = request.command;
		if (!command_authorized(command, connection->peer_credentials())) {
			response.status = ipc::Status::permission_denied;
			response.message = "Data Sender command is not authorized for this peer";
			connection->post_send(ipc::encode_response(
				response, frame.correlation_id, command));
			return;
		}
		switch (command) {
		case ipc::Command::get_status:
			response.json = encode_json(status_snapshot());
			break;
		case ipc::Command::list_artifacts: {
			mnc::datalogger::ArtifactListFilter filter;
			filter.job_id = request.job_id.empty()
				? std::nullopt : std::optional{request.job_id};
			filter.state = artifact_state(request.state);
			filter.start = request.start_nanoseconds;
			filter.end = request.end_nanoseconds;
			filter.offset = request.offset;
			filter.limit = request.limit;
			ipc::ArtifactList result;
			result.offset = request.offset;
			for (const auto &item : outbox_.list(filter))
				result.artifacts.push_back(artifact_dto(item));
			result.returned = result.artifacts.size();
			response.json = encode_json(result);
			break;
		}
		case ipc::Command::get_artifact: {
			const auto source = outbox_.artifact(request.id);
			ipc::ArtifactDetail result;
			result.artifact = artifact_dto(source.artifact);
			for (const auto &delivery : source.deliveries)
				result.deliveries.push_back({delivery.channel_id,
					delivery_state(delivery.state), delivery.attempt_count,
					delivery.next_attempt, delivery.last_attempt,
					delivery.remote_result, delivery.last_error});
			response.json = encode_json(result);
			break;
		}
		case ipc::Command::preview_artifact:
			if (request.limit == 0 || request.limit > 64u * 1024u)
				throw std::invalid_argument("preview limit must be 1..65536");
			response.content = outbox_.preview(request.id, request.limit);
			break;
		case ipc::Command::read_artifact_chunk: {
			if (request.limit == 0 || request.limit > 512u * 1024u)
				throw std::invalid_argument("chunk limit must be 1..524288");
			const auto chunk = outbox_.read_chunk(
				request.id, request.offset, request.limit);
			response.content = chunk.content;
			response.filename = chunk.filename;
			response.mime_type = chunk.mime_type;
			response.sha256 = chunk.sha256;
			response.total_size = chunk.total_size;
			response.end_of_file = chunk.end_of_file;
			break;
		}
		case ipc::Command::retry_artifacts:
			outbox_.retry(request.ids, clock_.now());
			wait_condition_.notify_all();
			break;
		case ipc::Command::delete_artifacts: {
			const auto result = outbox_.erase(request.ids,
				request.discard_unsent, clock_.now());
			response.json = encode_json(ipc::DeletionResult{
				result.deleted, result.discarded_deliveries});
			break;
		}
		case ipc::Command::test_channel:
			response.json = encode_json(test_channel(request.id));
			break;
		case ipc::Command::validate_channels:
			outbox_.validate_channels(request.ids);
			break;
		}
	} catch (const std::invalid_argument &error) {
		response.status = ipc::Status::invalid_request;
		response.message = error.what();
	} catch (const std::out_of_range &error) {
		response.status = ipc::Status::not_found;
		response.message = error.what();
	} catch (const mnc::datalogger::DatalogError &error) {
		response.status = ipc::Status::unavailable;
		response.message = error.what();
	} catch (const std::runtime_error &error) {
		response.status =
			(std::string_view(error.what()).contains("confirmation") ||
			 std::string_view(error.what()).contains("queued deliveries"))
			? ipc::Status::conflict : ipc::Status::internal_error;
		response.message = error.what();
	} catch (const std::exception &error) {
		response.status = ipc::Status::internal_error;
		response.message = error.what();
	}
	connection->post_send(ipc::encode_response(
		response, frame.correlation_id, command));
}

mnc::ServiceHealth DataSenderService::health() const
{
	if (worker_failed_) {
		std::scoped_lock lock(failure_mutex_);
		return {false, "Data Sender worker failed: " + failure_message_};
	}
	try {
		const auto status = status_snapshot();
		return {true, status.health + ": " + status.message};
	} catch (const std::exception &error) {
		return {false, "Data Sender status failed: " +
			std::string(error.what())};
	}
}

} // namespace msap1::datalogger::daemon
