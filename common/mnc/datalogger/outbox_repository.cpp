#include "mnc/datalogger/outbox_repository.hpp"

#include "mnc/storage/sqlite/sqlite_database.hpp"

#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <ranges>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace mnc::datalogger {
namespace {

using mnc::storage::sqlite::Database;
using mnc::storage::sqlite::Statement;
using mnc::storage::sqlite::Transaction;

constexpr std::size_t maximum_preview = 1024u * 1024u;

std::int32_t integer(DeliveryState state)
{
	return static_cast<std::int32_t>(state);
}

bool safe_filename(std::string_view value)
{
	if (value.empty() || value.size() > 240 || value.front() == '.' ||
	    value == "." || value == "..")
		return false;
	return std::ranges::all_of(value, [](unsigned char character) {
		return std::isalnum(character) != 0 || character == '-' ||
			character == '_' || character == '.';
	});
}

bool safe_identity(std::string_view value)
{
	if (value.empty() || value.size() > 160)
		return false;
	return std::ranges::all_of(value, [](unsigned char character) {
		return std::isalnum(character) != 0 || character == '-' ||
			character == '_';
	});
}

std::filesystem::path payload_path(const std::filesystem::path &root,
	bool local_only, std::string_view filename)
{
	if (!safe_filename(filename))
		throw std::invalid_argument("outbox filename is unsafe");
	return root / (local_only ? "archive" : "outbox") /
		std::string(filename);
}

std::string read_file(const std::filesystem::path &path,
	std::uint64_t expected_size)
{
	std::ifstream input(path, std::ios::binary);
	if (!input)
		throw DatalogError(DatalogErrorCode::StorageFailure,
			"artifact payload is unavailable");
	std::string result{std::istreambuf_iterator<char>(input),
		std::istreambuf_iterator<char>()};
	if (result.size() != expected_size)
		throw DatalogError(DatalogErrorCode::StorageFailure,
			"artifact payload size does not match its manifest");
	return result;
}

std::string sha256(std::string_view body)
{
	std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
	SHA256(reinterpret_cast<const unsigned char *>(body.data()), body.size(),
		digest.data());
	std::ostringstream result;
	result << std::hex << std::setfill('0');
	for (const auto byte : digest)
		result << std::setw(2) << static_cast<unsigned>(byte);
	return result.str();
}

void sync_directory(const std::filesystem::path &path)
{
	const auto descriptor = ::open(path.c_str(),
		O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (descriptor < 0)
		throw std::runtime_error("cannot open outbox directory: " +
			std::string(std::strerror(errno)));
	if (::fsync(descriptor) != 0) {
		const auto message = std::string(std::strerror(errno));
		(void)::close(descriptor);
		throw std::runtime_error("cannot sync outbox directory: " + message);
	}
	(void)::close(descriptor);
}

void atomic_write(const std::filesystem::path &path, std::string_view body)
{
	static std::atomic<std::uint64_t> counter{1};
	const auto temporary = path.parent_path() /
		("." + path.filename().string() + ".tmp." +
		 std::to_string(::getpid()) + "." +
		 std::to_string(counter.fetch_add(1)));
	const auto descriptor = ::open(temporary.c_str(),
		O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0640);
	if (descriptor < 0)
		throw DatalogError(DatalogErrorCode::StorageFailure,
			"cannot create an outbox temporary file");
	bool open = true;
	try {
		std::size_t offset = 0;
		while (offset < body.size()) {
			const auto written = ::write(descriptor, body.data() + offset,
				body.size() - offset);
			if (written < 0) {
				if (errno == EINTR)
					continue;
				throw std::runtime_error("cannot write outbox payload");
			}
			offset += static_cast<std::size_t>(written);
		}
		/* Persist the final restrictive mode with the payload contents. */
		if (::fchmod(descriptor, 0440) != 0 || ::fsync(descriptor) != 0)
			throw std::runtime_error("cannot sync outbox payload");
		if (::close(descriptor) != 0)
			throw std::runtime_error("cannot close outbox payload");
		open = false;
		if (::rename(temporary.c_str(), path.c_str()) != 0)
			throw std::runtime_error("cannot publish outbox payload");
		sync_directory(path.parent_path());
	} catch (const DatalogError &) {
		if (open)
			(void)::close(descriptor);
		std::error_code ignored;
		std::filesystem::remove(temporary, ignored);
		throw;
	} catch (const std::exception &error) {
		if (open)
			(void)::close(descriptor);
		std::error_code ignored;
		std::filesystem::remove(temporary, ignored);
		throw DatalogError(DatalogErrorCode::StorageFailure, error.what());
	}
}

std::string sanitize_result(std::string_view value)
{
	std::string result;
	result.reserve(std::min<std::size_t>(value.size(), 1024));
	for (const unsigned char character : value) {
		if (result.size() == 1024)
			break;
		if (character == '\r' || character == '\n' || character == '\t')
			result.push_back(' ');
		else if (character >= 0x20u && character != 0x7fu)
			result.push_back(static_cast<char>(character));
	}
	return result;
}

std::string json_string(std::string_view value)
{
	std::string result{"\""};
	for (const unsigned char character : value) {
		switch (character) {
		case '"': result += "\\\""; break;
		case '\\': result += "\\\\"; break;
		case '\n': result += "\\n"; break;
		case '\r': result += "\\r"; break;
		case '\t': result += "\\t"; break;
		default:
			if (character >= 0x20u)
				result.push_back(static_cast<char>(character));
		}
	}
	result.push_back('"');
	return result;
}

std::string job_snapshot(const GeneratedDataset &dataset,
	std::span<const std::string> channels, bool local_only)
{
	std::ostringstream result;
	result << "{\"schema\":\"mnc.meter.datalog.job-snapshot.v1\",";
	result << "\"job_id\":" << json_string(dataset.job_id) << ',';
	result << "\"job_revision\":\"" << dataset.job_revision << "\",";
	result << "\"source_period\":" << json_string(period_name(
		dataset.source_period)) << ',';
	result << "\"format\":" << json_string(content_format_name(
		dataset.format)) << ',';
	result << "\"local_only\":" << (local_only ? "true" : "false") << ',';
	result << "\"channels\":[";
	for (std::size_t index = 0; index < channels.size(); ++index)
		result << (index == 0 ? "" : ",") << json_string(channels[index]);
	result << "],\"columns\":[";
	for (std::size_t index = 0; index < dataset.columns.size(); ++index)
		result << (index == 0 ? "" : ",")
		       << json_string(dataset.columns[index].id);
	result << "]}";
	return result.str();
}

std::string extension_of(std::string_view filename)
{
	const auto separator = filename.rfind('.');
	return separator == std::string_view::npos
		? std::string{} : std::string(filename.substr(separator + 1));
}

} // namespace

class SqliteOutboxRepository::Implementation {
public:
	Implementation(std::filesystem::path owned_root, OutboxStoragePolicy owned_policy)
		: root(std::move(owned_root)), policy(owned_policy),
		  database(root / "manifest.sqlite3")
	{
	}

	std::filesystem::path root;
	OutboxStoragePolicy policy;
	mutable std::mutex mutex;
	Database database;
	bool initialized = false;

	[[nodiscard]] StorageGuardStatus storage_status() const;
	[[nodiscard]] ArtifactSummary summary(Statement &statement) const;
	void recover_locked();
	void finalize_completed_locked(std::string_view artifact_id);
	void finish_pending_deletions_locked();
	void prune_completed_locked(UtcNanoseconds now);
};

SqliteOutboxRepository::SqliteOutboxRepository(std::filesystem::path root,
	OutboxStoragePolicy policy)
	: implementation_(std::make_unique<Implementation>(std::move(root), policy))
{
}

SqliteOutboxRepository::~SqliteOutboxRepository() = default;

void SqliteOutboxRepository::initialize()
{
	auto &self = *implementation_;
	std::scoped_lock lock(self.mutex);
	if (self.initialized)
		return;
	std::filesystem::create_directories(self.root / "outbox");
	std::filesystem::create_directories(self.root / "archive");
	self.database.execute("PRAGMA journal_mode=WAL");
	self.database.execute("PRAGMA synchronous=FULL");
	self.database.execute(R"sql(
CREATE TABLE IF NOT EXISTS artifacts(
 artifact_id TEXT PRIMARY KEY,
 job_id TEXT NOT NULL,
 job_revision INTEGER NOT NULL,
 filename TEXT NOT NULL UNIQUE,
 mime_type TEXT NOT NULL,
 content_format TEXT NOT NULL,
 checksum_sha256 TEXT NOT NULL,
 size_bytes INTEGER NOT NULL,
 source_start_ns INTEGER NOT NULL,
 source_end_ns INTEGER NOT NULL,
 generated_at_ns INTEGER NOT NULL,
 created_at_ns INTEGER NOT NULL,
 local_only INTEGER NOT NULL,
 payload_present INTEGER NOT NULL,
 job_snapshot_json TEXT NOT NULL,
 recovery_error TEXT NOT NULL DEFAULT '',
 delete_pending INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS deliveries(
 artifact_id TEXT NOT NULL REFERENCES artifacts(artifact_id) ON DELETE CASCADE,
 channel_id TEXT NOT NULL,
 state INTEGER NOT NULL,
 attempt_count INTEGER NOT NULL DEFAULT 0,
 next_attempt_ns INTEGER NOT NULL DEFAULT 0,
 last_attempt_ns INTEGER NOT NULL DEFAULT 0,
 remote_result TEXT NOT NULL DEFAULT '',
 last_error TEXT NOT NULL DEFAULT '',
 PRIMARY KEY(artifact_id, channel_id)
);
CREATE INDEX IF NOT EXISTS deliveries_due_idx
 ON deliveries(state, next_attempt_ns, artifact_id, channel_id);
CREATE TABLE IF NOT EXISTS job_watermarks(
 job_id TEXT PRIMARY KEY,
 job_revision INTEGER NOT NULL,
 completed_through_ns INTEGER NOT NULL,
 updated_at_ns INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS administrative_audit(
 id INTEGER PRIMARY KEY AUTOINCREMENT,
 artifact_id TEXT NOT NULL,
 action TEXT NOT NULL,
 occurred_at_ns INTEGER NOT NULL,
 detail TEXT NOT NULL
);
)sql");
	self.finish_pending_deletions_locked();
	self.recover_locked();
	self.initialized = true;
}

void SqliteOutboxRepository::update_storage_policy(OutboxStoragePolicy policy)
{
	if (policy.maximum_bytes == 0 ||
	    policy.completed_metadata_retention <= 0)
		throw std::invalid_argument("outbox storage quota must be nonzero");
	auto &self = *implementation_;
	std::scoped_lock lock(self.mutex);
	self.policy = policy;
}

StorageGuardStatus SqliteOutboxRepository::Implementation::storage_status() const
{
	StorageGuardStatus result;
	result.maximum_bytes = policy.maximum_bytes;
	result.minimum_free_bytes = policy.minimum_free_bytes;
	for (const auto &directory : {root / "outbox", root / "archive"}) {
		std::error_code error;
		for (std::filesystem::directory_iterator iterator(directory, error), end;
		     !error && iterator != end; iterator.increment(error)) {
			const auto &entry = *iterator;
			if (!entry.is_regular_file(error) ||
			    entry.path().filename().string().contains(".tmp."))
				continue;
			result.payload_bytes += entry.file_size(error);
			if (error)
				break;
		}
		if (error)
			throw DatalogError(DatalogErrorCode::StorageFailure,
				"cannot inspect Data Sender payload storage");
	}
	const auto space = std::filesystem::space(root);
	result.available_bytes = space.available;
	if (result.payload_bytes >= result.maximum_bytes) {
		result.generation_allowed = false;
		result.blocking_reason = "combined outbox/archive quota reached";
	} else if (result.available_bytes <= result.minimum_free_bytes) {
		result.generation_allowed = false;
		result.blocking_reason = "filesystem free-space reserve reached";
	}
	return result;
}

void SqliteOutboxRepository::enqueue(const GeneratedDataset &dataset,
	const GeneratedContent &content,
	std::span<const std::string> channel_ids, bool local_only)
{
	auto &self = *implementation_;
	std::scoped_lock lock(self.mutex);
	if (!self.initialized)
		throw std::logic_error("outbox repository is not initialized");
	if (!safe_identity(dataset.artifact_id) ||
	    content.artifact_id != dataset.artifact_id ||
	    !safe_filename(content.filename) ||
	    content.filename != dataset.artifact_id + "." + content.extension ||
	    (content.extension != "json" && content.extension != "csv") ||
	    sha256(content.body) != content.sha256 || content.sha256.size() != 64)
		throw DatalogError(DatalogErrorCode::StorageFailure,
			"generated artifact identity or checksum is invalid");
	if (local_only != channel_ids.empty())
		throw DatalogError(DatalogErrorCode::InvalidConfiguration,
			"artifact destination is invalid");
	std::unordered_set<std::string_view> unique_channels;
	for (const auto &channel : channel_ids)
		if (!safe_identity(channel) || !unique_channels.insert(channel).second)
			throw DatalogError(DatalogErrorCode::InvalidConfiguration,
				"artifact channel selection is invalid");

	{
		auto existing = self.database.prepare(
			"SELECT filename,checksum_sha256,size_bytes,source_start_ns,"
			"source_end_ns,job_revision FROM artifacts WHERE artifact_id=?");
		existing.bind(1, dataset.artifact_id);
		if (existing.step()) {
			if (existing.text(0) == content.filename &&
			    existing.text(1) == content.sha256 &&
			    existing.integer(2) == static_cast<std::int64_t>(content.body.size()) &&
			    existing.integer(3) == dataset.artifact_window.start &&
			    existing.integer(4) == dataset.artifact_window.end &&
			    existing.integer(5) == static_cast<std::int64_t>(dataset.job_revision))
				return;
			throw DatalogError(DatalogErrorCode::StorageFailure,
				"artifact ID conflicts with a different manifest");
		}
	}

	const auto guard = self.storage_status();
	if (!guard.generation_allowed ||
	    content.body.size() > guard.maximum_bytes - guard.payload_bytes ||
	    guard.available_bytes <= guard.minimum_free_bytes ||
	    content.body.size() > guard.available_bytes - guard.minimum_free_bytes)
		throw DatalogError(DatalogErrorCode::StorageFailure,
			guard.blocking_reason.empty()
				? "artifact would exceed the Data Sender storage guard"
				: guard.blocking_reason);

	const auto path = payload_path(self.root, local_only, content.filename);
	if (std::filesystem::exists(path))
		throw DatalogError(DatalogErrorCode::StorageFailure,
			"unmanifested artifact filename already exists");
	atomic_write(path, content.body);
	try {
		Transaction transaction(self.database);
		auto insert = self.database.prepare(R"sql(
INSERT INTO artifacts(artifact_id,job_id,job_revision,filename,mime_type,
 content_format,checksum_sha256,size_bytes,source_start_ns,source_end_ns,
 generated_at_ns,created_at_ns,local_only,payload_present,job_snapshot_json)
VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
)sql");
		insert.bind(1, dataset.artifact_id);
		insert.bind(2, dataset.job_id);
		insert.bind(3, dataset.job_revision);
		insert.bind(4, content.filename);
		insert.bind(5, content.mime_type);
		insert.bind(6, content.extension);
		insert.bind(7, content.sha256);
		insert.bind(8, static_cast<std::uint64_t>(content.body.size()));
		insert.bind(9, dataset.artifact_window.start);
		insert.bind(10, dataset.artifact_window.end);
		insert.bind(11, dataset.generated_at);
		insert.bind(12, dataset.generated_at);
		insert.bind(13, static_cast<std::int32_t>(local_only ? 1 : 0));
		insert.bind(14, std::int32_t{1});
		insert.bind(15, job_snapshot(dataset, channel_ids, local_only));
		insert.execute();
		auto delivery = self.database.prepare(
			"INSERT INTO deliveries(artifact_id,channel_id,state) VALUES(?,?,?)");
		for (const auto &channel : channel_ids) {
			delivery.bind(1, dataset.artifact_id);
			delivery.bind(2, channel);
			delivery.bind(3, integer(DeliveryState::Pending));
			delivery.execute();
			delivery.reset();
		}
		transaction.commit();
	} catch (...) {
		/* The complete file intentionally remains. Startup recovery creates a
		 * blocked manifest instead of silently discarding possible customer data. */
		throw;
	}
}

std::vector<QueuedDelivery> SqliteOutboxRepository::due(
	UtcNanoseconds now, std::size_t limit) const
{
	if (limit == 0 || limit > maximum_artifact_list_page)
		throw std::invalid_argument("outbox due limit is invalid");
	auto &self = *implementation_;
	std::scoped_lock lock(self.mutex);
	auto query = self.database.prepare(R"sql(
SELECT d.artifact_id,d.channel_id,d.state,d.attempt_count,d.next_attempt_ns
FROM deliveries d JOIN artifacts a ON a.artifact_id=d.artifact_id
WHERE a.payload_present=1 AND a.delete_pending=0
 AND d.state IN (?,?,?) AND d.next_attempt_ns<=?
ORDER BY d.next_attempt_ns,d.artifact_id,d.channel_id LIMIT ?
)sql");
	query.bind(1, integer(DeliveryState::Pending));
	query.bind(2, integer(DeliveryState::RetryWait));
	query.bind(3, integer(DeliveryState::InFlight));
	query.bind(4, now);
	query.bind(5, static_cast<std::uint64_t>(limit));
	std::vector<QueuedDelivery> result;
	while (query.step())
		result.push_back({query.text(0), query.text(1),
			static_cast<DeliveryState>(query.integer(2)),
			static_cast<std::uint32_t>(query.integer(3)), query.integer(4),
			0, {}, {}});
	return result;
}

std::vector<QueuedDelivery> SqliteOutboxRepository::claim_due(
	UtcNanoseconds now, UtcNanoseconds lease_until, std::size_t limit)
{
	if (limit == 0 || limit > maximum_artifact_list_page || lease_until <= now)
		throw std::invalid_argument("outbox delivery lease is invalid");
	auto &self = *implementation_;
	std::scoped_lock lock(self.mutex);
	Transaction transaction(self.database);
	auto query = self.database.prepare(R"sql(
SELECT d.artifact_id,d.channel_id,d.state,d.attempt_count,d.next_attempt_ns
FROM deliveries d JOIN artifacts a ON a.artifact_id=d.artifact_id
WHERE a.payload_present=1 AND a.delete_pending=0
 AND ((d.state IN (?,?) AND d.next_attempt_ns<=?)
      OR (d.state=? AND d.next_attempt_ns<=?))
ORDER BY d.next_attempt_ns,d.artifact_id,d.channel_id LIMIT ?
)sql");
	query.bind(1, integer(DeliveryState::Pending));
	query.bind(2, integer(DeliveryState::RetryWait));
	query.bind(3, now);
	query.bind(4, integer(DeliveryState::InFlight));
	query.bind(5, now);
	query.bind(6, static_cast<std::uint64_t>(limit));
	std::vector<QueuedDelivery> result;
	while (query.step())
		result.push_back({query.text(0), query.text(1),
			DeliveryState::InFlight,
			static_cast<std::uint32_t>(query.integer(3)), lease_until,
			0, {}, {}});
	auto claim = self.database.prepare(R"sql(
UPDATE deliveries SET state=?,next_attempt_ns=?
WHERE artifact_id=? AND channel_id=?
)sql");
	for (const auto &delivery : result) {
		claim.bind(1, integer(DeliveryState::InFlight));
		claim.bind(2, lease_until);
		claim.bind(3, delivery.artifact_id);
		claim.bind(4, delivery.channel_id);
		claim.execute();
		claim.reset();
	}
	transaction.commit();
	return result;
}

GeneratedContent SqliteOutboxRepository::content(
	std::string_view artifact_id) const
{
	auto &self = *implementation_;
	std::scoped_lock lock(self.mutex);
	auto query = self.database.prepare(R"sql(
SELECT filename,mime_type,content_format,checksum_sha256,size_bytes,
 local_only,payload_present FROM artifacts
WHERE artifact_id=? AND delete_pending=0
)sql");
	query.bind(1, artifact_id);
	if (!query.step())
		throw std::out_of_range("unknown artifact ID");
	if (query.integer(6) == 0)
		throw DatalogError(DatalogErrorCode::StorageFailure,
			"artifact payload is not retained");
	GeneratedContent result;
	result.artifact_id = std::string(artifact_id);
	result.filename = query.text(0);
	result.mime_type = query.text(1);
	result.extension = query.text(2);
	result.sha256 = query.text(3);
	result.body = read_file(payload_path(self.root, query.integer(5) != 0,
		result.filename), static_cast<std::uint64_t>(query.integer(4)));
	if (sha256(result.body) != result.sha256)
		throw DatalogError(DatalogErrorCode::StorageFailure,
			"artifact payload checksum does not match its manifest");
	return result;
}

void SqliteOutboxRepository::record_result(const QueuedDelivery &delivery,
	const DeliveryResult &result, UtcNanoseconds attempted_at,
	UtcNanoseconds next_attempt)
{
	auto &self = *implementation_;
	std::scoped_lock lock(self.mutex);
	DeliveryState state = DeliveryState::RetryWait;
	switch (result.disposition) {
	case DeliveryDisposition::Succeeded: state = DeliveryState::Succeeded; break;
	case DeliveryDisposition::Retryable: state = DeliveryState::RetryWait; break;
	case DeliveryDisposition::Blocked: state = DeliveryState::Blocked; break;
	}
	Transaction transaction(self.database);
	auto update = self.database.prepare(R"sql(
UPDATE deliveries SET state=?,attempt_count=attempt_count+1,
 next_attempt_ns=?,last_attempt_ns=?,remote_result=?,last_error=?
WHERE artifact_id=? AND channel_id=? AND state NOT IN (?,?)
)sql");
	update.bind(1, integer(state));
	update.bind(2, state == DeliveryState::RetryWait ? next_attempt : 0);
	update.bind(3, attempted_at);
	update.bind(4, sanitize_result(result.remote_result));
	update.bind(5, sanitize_result(result.sanitized_error));
	update.bind(6, delivery.artifact_id);
	update.bind(7, delivery.channel_id);
	update.bind(8, integer(DeliveryState::Succeeded));
	update.bind(9, integer(DeliveryState::AdministrativelyDiscarded));
	update.execute();
	if (self.database.changes() != 1)
		throw std::runtime_error("delivery is unknown or already finalized");
	transaction.commit();
	self.finalize_completed_locked(delivery.artifact_id);
	self.prune_completed_locked(attempted_at);
}

ArtifactSummary SqliteOutboxRepository::Implementation::summary(
	Statement &query) const
{
	ArtifactSummary result;
	result.artifact_id = query.text(0);
	result.job_id = query.text(1);
	result.job_revision = static_cast<std::uint64_t>(query.integer(2));
	result.filename = query.text(3);
	result.mime_type = query.text(4);
	result.sha256 = query.text(5);
	result.size_bytes = static_cast<std::uint64_t>(query.integer(6));
	result.source_window = {query.integer(7), query.integer(8)};
	result.generated_at = query.integer(9);
	result.created_at = query.integer(10);
	result.local_only = query.integer(11) != 0;
	result.payload_present = query.integer(12) != 0;
	result.recovery_error = query.text(13);
	result.delivery_count = static_cast<std::uint32_t>(query.integer(14));
	result.succeeded_count = static_cast<std::uint32_t>(query.integer(15));
	result.blocked_count = static_cast<std::uint32_t>(query.integer(16));
	const auto damaged_payload =
		result.recovery_error == "payload size mismatch" ||
		result.recovery_error == "payload cannot be read" ||
		result.recovery_error == "payload checksum mismatch";
	if ((!result.payload_present && !result.recovery_error.empty()) ||
	    damaged_payload)
		result.state = ArtifactState::MissingPayload;
	else if (result.local_only)
		result.state = ArtifactState::LocalOnly;
	else if (result.delivery_count != 0 &&
		 result.succeeded_count == result.delivery_count)
		result.state = ArtifactState::Succeeded;
	else if (result.blocked_count != 0 ||
		 (result.delivery_count == 0 && !result.recovery_error.empty()))
		result.state = ArtifactState::Blocked;
	else if (result.succeeded_count != 0)
		result.state = ArtifactState::PartiallyDelivered;
	else
		result.state = ArtifactState::Pending;
	return result;
}

std::vector<ArtifactSummary> SqliteOutboxRepository::list(
	const ArtifactListFilter &filter) const
{
	if (filter.limit == 0 ||
	    filter.limit > maximum_artifact_list_page ||
	    filter.offset > 100000)
		throw std::invalid_argument("artifact list page is invalid");
	auto &self = *implementation_;
	std::scoped_lock lock(self.mutex);
	std::string sql = R"sql(
SELECT a.artifact_id,a.job_id,a.job_revision,a.filename,a.mime_type,
 a.checksum_sha256,a.size_bytes,a.source_start_ns,a.source_end_ns,
 a.generated_at_ns,a.created_at_ns,a.local_only,a.payload_present,
 a.recovery_error,COUNT(d.channel_id),
 COALESCE(SUM(CASE WHEN d.state=3 THEN 1 ELSE 0 END),0),
 COALESCE(SUM(CASE WHEN d.state=2 THEN 1 ELSE 0 END),0)
FROM artifacts a LEFT JOIN deliveries d ON d.artifact_id=a.artifact_id
WHERE a.delete_pending=0
)sql";
	if (filter.job_id)
		sql += " AND a.job_id=?";
	if (filter.start)
		sql += " AND a.source_end_ns>?";
	if (filter.end)
		sql += " AND a.source_start_ns<?";
	sql += " GROUP BY a.artifact_id ORDER BY a.created_at_ns DESC,a.artifact_id DESC";
	auto query = self.database.prepare(sql);
	int binding = 1;
	if (filter.job_id)
		query.bind(binding++, *filter.job_id);
	if (filter.start)
		query.bind(binding++, *filter.start);
	if (filter.end)
		query.bind(binding++, *filter.end);
	std::vector<ArtifactSummary> result;
	std::size_t accepted = 0;
	while (query.step()) {
		auto item = self.summary(query);
		if (filter.state && item.state != *filter.state)
			continue;
		if (accepted++ < filter.offset)
			continue;
		result.push_back(std::move(item));
		if (result.size() == filter.limit)
			break;
	}
	return result;
}

ArtifactDetail SqliteOutboxRepository::artifact(
	std::string_view artifact_id) const
{
	auto &self = *implementation_;
	std::scoped_lock lock(self.mutex);
	auto query = self.database.prepare(R"sql(
SELECT a.artifact_id,a.job_id,a.job_revision,a.filename,a.mime_type,
 a.checksum_sha256,a.size_bytes,a.source_start_ns,a.source_end_ns,
 a.generated_at_ns,a.created_at_ns,a.local_only,a.payload_present,
 a.recovery_error,COUNT(d.channel_id),
 COALESCE(SUM(CASE WHEN d.state=3 THEN 1 ELSE 0 END),0),
 COALESCE(SUM(CASE WHEN d.state=2 THEN 1 ELSE 0 END),0)
FROM artifacts a LEFT JOIN deliveries d ON d.artifact_id=a.artifact_id
WHERE a.artifact_id=? AND a.delete_pending=0 GROUP BY a.artifact_id
)sql");
	query.bind(1, artifact_id);
	if (!query.step())
		throw std::out_of_range("unknown artifact ID");
	ArtifactDetail result;
	result.artifact = self.summary(query);
	auto deliveries = self.database.prepare(R"sql(
	SELECT artifact_id,channel_id,state,attempt_count,next_attempt_ns,
	 last_attempt_ns,remote_result,last_error
	FROM deliveries WHERE artifact_id=? ORDER BY channel_id
)sql");
	deliveries.bind(1, artifact_id);
	while (deliveries.step())
		result.deliveries.push_back({deliveries.text(0), deliveries.text(1),
			static_cast<DeliveryState>(deliveries.integer(2)),
			static_cast<std::uint32_t>(deliveries.integer(3)),
			deliveries.integer(4), deliveries.integer(5), deliveries.text(6),
			deliveries.text(7)});
	return result;
}

std::string SqliteOutboxRepository::preview(std::string_view artifact_id,
	std::size_t maximum_bytes) const
{
	if (maximum_bytes == 0 || maximum_bytes > maximum_preview)
		throw std::invalid_argument("artifact preview limit is invalid");
	return read_chunk(artifact_id, 0, maximum_bytes).content;
}

ArtifactContentChunk SqliteOutboxRepository::read_chunk(
	std::string_view artifact_id, std::uint64_t offset,
	std::size_t maximum_bytes) const
{
	if (maximum_bytes == 0 || maximum_bytes > maximum_preview)
		throw std::invalid_argument("artifact chunk limit is invalid");
	auto &self = *implementation_;
	std::scoped_lock lock(self.mutex);
	auto query = self.database.prepare(R"sql(
SELECT filename,mime_type,checksum_sha256,size_bytes,local_only,payload_present
FROM artifacts WHERE artifact_id=? AND delete_pending=0
)sql");
	query.bind(1, artifact_id);
	if (!query.step())
		throw std::out_of_range("unknown artifact ID");
	if (query.integer(5) == 0)
		throw DatalogError(DatalogErrorCode::StorageFailure,
			"artifact payload is not retained");
	ArtifactContentChunk result;
	result.artifact_id = std::string(artifact_id);
	result.filename = query.text(0);
	result.mime_type = query.text(1);
	result.sha256 = query.text(2);
	result.total_size = static_cast<std::uint64_t>(query.integer(3));
	result.offset = offset;
	if (offset > result.total_size)
		throw std::invalid_argument("artifact chunk offset is beyond end of file");
	const auto amount = static_cast<std::size_t>(std::min<std::uint64_t>(
		maximum_bytes, result.total_size - offset));
	std::ifstream input(payload_path(self.root, query.integer(4) != 0,
		result.filename), std::ios::binary);
	if (!input)
		throw DatalogError(DatalogErrorCode::StorageFailure,
			"artifact payload is unavailable");
	input.seekg(static_cast<std::streamoff>(offset));
	result.content.resize(amount);
	input.read(result.content.data(), static_cast<std::streamsize>(amount));
	if (static_cast<std::size_t>(input.gcount()) != amount)
		throw DatalogError(DatalogErrorCode::StorageFailure,
			"artifact payload changed while reading");
	result.end_of_file = offset + amount == result.total_size;
	return result;
}

void SqliteOutboxRepository::retry(std::span<const std::string> artifact_ids,
	UtcNanoseconds now)
{
	if (artifact_ids.empty() || artifact_ids.size() > 500)
		throw std::invalid_argument("artifact retry selection is invalid");
	auto &self = *implementation_;
	std::scoped_lock lock(self.mutex);
	Transaction transaction(self.database);
	auto exists = self.database.prepare(
		"SELECT 1 FROM artifacts WHERE artifact_id=? AND delete_pending=0");
	auto update = self.database.prepare(R"sql(
UPDATE deliveries SET state=?,next_attempt_ns=?,last_error=''
WHERE artifact_id=? AND state IN (?,?,?)
)sql");
	for (const auto &artifact_id : artifact_ids) {
		exists.bind(1, artifact_id);
		if (!exists.step())
			throw std::out_of_range("unknown artifact ID");
		exists.reset();
		update.bind(1, integer(DeliveryState::Pending));
		update.bind(2, now);
		update.bind(3, artifact_id);
		update.bind(4, integer(DeliveryState::Pending));
		update.bind(5, integer(DeliveryState::RetryWait));
		update.bind(6, integer(DeliveryState::Blocked));
		update.execute();
		update.reset();
	}
	transaction.commit();
}

void SqliteOutboxRepository::retry_channel(std::string_view channel_id,
	UtcNanoseconds now)
{
	if (!safe_identity(channel_id))
		throw std::invalid_argument("data channel retry identity is invalid");
	auto &self = *implementation_;
	std::scoped_lock lock(self.mutex);
	auto update = self.database.prepare(R"sql(
UPDATE deliveries SET state=?,next_attempt_ns=?,last_error=''
WHERE channel_id=? AND state=?
)sql");
	update.bind(1, integer(DeliveryState::Pending));
	update.bind(2, now);
	update.bind(3, channel_id);
	update.bind(4, integer(DeliveryState::Blocked));
	update.execute();
}

void SqliteOutboxRepository::validate_channels(
	std::span<const std::string> available_channel_ids) const
{
	std::unordered_set<std::string_view> available;
	for (const auto &id : available_channel_ids)
		if (!safe_identity(id) || !available.insert(id).second)
			throw std::invalid_argument(
				"candidate data channel identities are invalid");
	auto &self = *implementation_;
	std::scoped_lock lock(self.mutex);
	auto query = self.database.prepare(R"sql(
SELECT DISTINCT d.channel_id
FROM deliveries d JOIN artifacts a ON a.artifact_id=d.artifact_id
WHERE a.delete_pending=0 AND d.state NOT IN (?,?)
ORDER BY d.channel_id
)sql");
	query.bind(1, integer(DeliveryState::Succeeded));
	query.bind(2, integer(DeliveryState::AdministrativelyDiscarded));
	std::vector<std::string> missing;
	while (query.step()) {
		auto id = query.text(0);
		if (!available.contains(id))
			missing.push_back(std::move(id));
	}
	if (!missing.empty()) {
		std::string message =
			"data channels cannot be deleted while queued deliveries reference: ";
		for (std::size_t index = 0; index < missing.size(); ++index)
			message += (index == 0 ? "" : ", ") + missing[index];
		throw std::runtime_error(std::move(message));
	}
}

void SqliteOutboxRepository::prune_completed(UtcNanoseconds now)
{
	auto &self = *implementation_;
	std::scoped_lock lock(self.mutex);
	self.prune_completed_locked(now);
}

ArtifactDeletionResult SqliteOutboxRepository::erase(
	std::span<const std::string> artifact_ids, bool discard_unsent,
	UtcNanoseconds now)
{
	if (artifact_ids.empty() || artifact_ids.size() > 500)
		throw std::invalid_argument("artifact deletion selection is invalid");
	auto &self = *implementation_;
	std::scoped_lock lock(self.mutex);
	struct Target {
		std::string id;
		std::string filename;
		bool local_only = false;
		bool payload_present = false;
		std::uint64_t unsent = 0;
	};
	std::vector<Target> targets;
	for (const auto &id : artifact_ids) {
		auto query = self.database.prepare(R"sql(
SELECT a.filename,a.local_only,a.payload_present,
 COALESCE(SUM(CASE WHEN d.state NOT IN (3,4) THEN 1 ELSE 0 END),0),
 COUNT(d.channel_id) FROM artifacts a
LEFT JOIN deliveries d ON d.artifact_id=a.artifact_id
WHERE a.artifact_id=? AND a.delete_pending=0 GROUP BY a.artifact_id
)sql");
		query.bind(1, id);
		if (!query.step())
			throw std::out_of_range("unknown artifact ID");
		Target target{id, query.text(0), query.integer(1) != 0,
			query.integer(2) != 0, static_cast<std::uint64_t>(query.integer(3))};
		if (!target.local_only && query.integer(4) == 0)
			target.unsent = 1; // recovered payload with no delivery manifest
		if (target.unsent != 0 && !discard_unsent)
			throw std::runtime_error(
				"deleting unsent artifacts requires discard_unsent confirmation");
		targets.push_back(std::move(target));
	}

	ArtifactDeletionResult result;
	{
		Transaction transaction(self.database);
		auto discard = self.database.prepare(R"sql(
UPDATE deliveries SET state=?,next_attempt_ns=0,last_error='administrator discarded'
WHERE artifact_id=? AND state NOT IN (?,?)
)sql");
		auto mark = self.database.prepare(
			"UPDATE artifacts SET delete_pending=1 WHERE artifact_id=?");
		auto audit = self.database.prepare(R"sql(
INSERT INTO administrative_audit(artifact_id,action,occurred_at_ns,detail)
VALUES(?,?,?,?)
)sql");
		for (const auto &target : targets) {
			if (target.unsent != 0) {
				discard.bind(1, integer(DeliveryState::AdministrativelyDiscarded));
				discard.bind(2, target.id);
				discard.bind(3, integer(DeliveryState::Succeeded));
				discard.bind(4,
					integer(DeliveryState::AdministrativelyDiscarded));
				discard.execute();
				result.discarded_deliveries += self.database.changes();
				discard.reset();
			}
			mark.bind(1, target.id);
			mark.execute();
			mark.reset();
			audit.bind(1, target.id);
			audit.bind(2, target.unsent == 0 ? "delete" : "discard_unsent");
			audit.bind(3, now);
			audit.bind(4, target.filename);
			audit.execute();
			audit.reset();
		}
		transaction.commit();
	}
	for (const auto &target : targets) {
		if (target.payload_present) {
			std::error_code error;
			std::filesystem::remove(payload_path(self.root, target.local_only,
				target.filename), error);
			if (error)
				throw DatalogError(DatalogErrorCode::StorageFailure,
					"cannot remove an administratively deleted payload");
		}
	}
	{
		Transaction transaction(self.database);
		auto remove = self.database.prepare(
			"DELETE FROM artifacts WHERE artifact_id=? AND delete_pending=1");
		for (const auto &target : targets) {
			remove.bind(1, target.id);
			remove.execute();
			result.deleted += self.database.changes();
			remove.reset();
		}
		transaction.commit();
	}
	return result;
}

OutboxStatus SqliteOutboxRepository::status() const
{
	auto &self = *implementation_;
	std::scoped_lock lock(self.mutex);
	OutboxStatus result;
	auto artifacts = self.database.prepare(R"sql(
SELECT local_only,COUNT(*),COALESCE(SUM(size_bytes),0)
FROM artifacts WHERE delete_pending=0 AND payload_present=1 GROUP BY local_only
)sql");
	while (artifacts.step()) {
		if (artifacts.integer(0) != 0) {
			result.archive_count = artifacts.integer(1);
			result.archive_bytes = artifacts.integer(2);
		} else {
			result.outbox_count = artifacts.integer(1);
			result.outbox_bytes = artifacts.integer(2);
		}
	}
	auto metadata = self.database.prepare(R"sql(
SELECT
 COUNT(*),
 COALESCE(SUM(CASE WHEN a.local_only=0 AND a.payload_present=0
   AND a.recovery_error='' AND EXISTS(
    SELECT 1 FROM deliveries d WHERE d.artifact_id=a.artifact_id)
   AND NOT EXISTS(
    SELECT 1 FROM deliveries d WHERE d.artifact_id=a.artifact_id
      AND d.state<>3)
  THEN 1 ELSE 0 END),0),
 COALESCE(SUM(CASE WHEN
   (a.payload_present=0 AND a.recovery_error<>'') OR
   a.recovery_error IN ('payload size mismatch','payload cannot be read',
     'payload checksum mismatch')
  THEN 1 ELSE 0 END),0)
FROM artifacts a WHERE a.delete_pending=0
)sql");
	if (metadata.step()) {
		result.artifact_count = metadata.integer(0);
		result.completed_metadata_count = metadata.integer(1);
		result.missing_payload_count = metadata.integer(2);
	}
	auto deliveries = self.database.prepare(R"sql(
SELECT
 COALESCE(SUM(CASE WHEN state IN (0,1,5) THEN 1 ELSE 0 END),0),
 COALESCE(SUM(CASE WHEN state=2 THEN 1 ELSE 0 END),0),
 MIN(CASE WHEN state IN (0,1,2,5) THEN a.created_at_ns ELSE NULL END)
FROM deliveries d JOIN artifacts a ON a.artifact_id=d.artifact_id
WHERE a.delete_pending=0
)sql");
	if (deliveries.step()) {
		result.pending_delivery_count = deliveries.integer(0);
		result.blocked_delivery_count = deliveries.integer(1);
		const auto oldest = deliveries.integer(2);
		if (oldest != 0)
			result.oldest_pending_created_at = oldest;
	}
	result.storage = self.storage_status();
	return result;
}

std::optional<JobWatermark> SqliteOutboxRepository::watermark(
	std::string_view job_id) const
{
	auto &self = *implementation_;
	std::scoped_lock lock(self.mutex);
	auto query = self.database.prepare(
		"SELECT job_id,job_revision,completed_through_ns FROM job_watermarks "
		"WHERE job_id=?");
	query.bind(1, job_id);
	if (!query.step())
		return std::nullopt;
	return JobWatermark{query.text(0),
		static_cast<std::uint64_t>(query.integer(1)), query.integer(2)};
}

void SqliteOutboxRepository::store_watermark(
	const JobWatermark &watermark, UtcNanoseconds updated_at)
{
	if (!safe_identity(watermark.job_id) || watermark.job_revision == 0)
		throw std::invalid_argument("job watermark identity is invalid");
	auto &self = *implementation_;
	std::scoped_lock lock(self.mutex);
	auto upsert = self.database.prepare(R"sql(
INSERT INTO job_watermarks(job_id,job_revision,completed_through_ns,updated_at_ns)
VALUES(?,?,?,?)
ON CONFLICT(job_id) DO UPDATE SET job_revision=excluded.job_revision,
 completed_through_ns=excluded.completed_through_ns,
 updated_at_ns=excluded.updated_at_ns
)sql");
	upsert.bind(1, watermark.job_id);
	upsert.bind(2, watermark.job_revision);
	upsert.bind(3, watermark.completed_through);
	upsert.bind(4, updated_at);
	upsert.execute();
}

void SqliteOutboxRepository::Implementation::finalize_completed_locked(
	std::string_view artifact_id)
{
	auto query = database.prepare(R"sql(
SELECT a.filename,a.local_only,a.payload_present,COUNT(d.channel_id),
 COALESCE(SUM(CASE WHEN d.state=3 THEN 1 ELSE 0 END),0)
FROM artifacts a LEFT JOIN deliveries d ON d.artifact_id=a.artifact_id
WHERE a.artifact_id=? AND a.delete_pending=0 GROUP BY a.artifact_id
)sql");
	query.bind(1, artifact_id);
	if (!query.step() || query.integer(1) != 0 || query.integer(2) == 0 ||
	    query.integer(3) == 0 || query.integer(3) != query.integer(4))
		return;
	std::error_code error;
	std::filesystem::remove(payload_path(root, false, query.text(0)), error);
	if (error)
		return;
	auto update = database.prepare(
		"UPDATE artifacts SET payload_present=0 WHERE artifact_id=?");
	update.bind(1, artifact_id);
	update.execute();
	sync_directory(root / "outbox");
}

void SqliteOutboxRepository::Implementation::finish_pending_deletions_locked()
{
	auto query = database.prepare(
		"SELECT artifact_id,filename,local_only,payload_present FROM artifacts "
		"WHERE delete_pending=1");
	std::vector<std::string> ids;
	while (query.step()) {
		ids.push_back(query.text(0));
		if (query.integer(3) != 0) {
			std::error_code ignored;
			std::filesystem::remove(payload_path(root, query.integer(2) != 0,
				query.text(1)), ignored);
		}
	}
	Transaction transaction(database);
	auto remove = database.prepare(
		"DELETE FROM artifacts WHERE artifact_id=? AND delete_pending=1");
	for (const auto &id : ids) {
		remove.bind(1, id);
		remove.execute();
		remove.reset();
	}
	transaction.commit();
}

void SqliteOutboxRepository::Implementation::prune_completed_locked(
	UtcNanoseconds now)
{
	if (now <= policy.completed_metadata_retention)
		return;
	const auto cutoff = now - policy.completed_metadata_retention;
	auto query = database.prepare(R"sql(
SELECT a.artifact_id
FROM artifacts a JOIN deliveries d ON d.artifact_id=a.artifact_id
WHERE a.local_only=0 AND a.payload_present=0 AND a.delete_pending=0
GROUP BY a.artifact_id
HAVING COUNT(d.channel_id)>0
 AND SUM(CASE WHEN d.state=? THEN 0 ELSE 1 END)=0
 AND MAX(d.last_attempt_ns)>0 AND MAX(d.last_attempt_ns)<=?
)sql");
	query.bind(1, integer(DeliveryState::Succeeded));
	query.bind(2, cutoff);
	std::vector<std::string> ids;
	while (query.step())
		ids.push_back(query.text(0));
	if (ids.empty())
		return;
	Transaction transaction(database);
	auto remove = database.prepare(
		"DELETE FROM artifacts WHERE artifact_id=? AND payload_present=0");
	for (const auto &id : ids) {
		remove.bind(1, id);
		remove.execute();
		remove.reset();
	}
	auto prune_audit = database.prepare(
		"DELETE FROM administrative_audit WHERE occurred_at_ns<=?");
	prune_audit.bind(1, cutoff);
	prune_audit.execute();
	transaction.commit();
}

void SqliteOutboxRepository::Implementation::recover_locked()
{
	const auto block_artifact = [this](std::string_view artifact_id,
		std::string_view error) {
		auto update = database.prepare(
			"UPDATE artifacts SET recovery_error=? WHERE artifact_id=?");
		update.bind(1, error);
		update.bind(2, artifact_id);
		update.execute();
		auto block = database.prepare(
			"UPDATE deliveries SET state=?,last_error=? WHERE artifact_id=? "
			"AND state NOT IN (?,?)");
		block.bind(1, integer(DeliveryState::Blocked));
		block.bind(2, error);
		block.bind(3, artifact_id);
		block.bind(4, integer(DeliveryState::Succeeded));
		block.bind(5, integer(DeliveryState::AdministrativelyDiscarded));
		block.execute();
	};
	struct Manifest {
		std::string id;
		std::string filename;
		std::string checksum;
		std::uint64_t size = 0;
		bool local_only = false;
		bool payload_present = false;
		std::uint64_t delivery_count = 0;
		std::uint64_t succeeded_count = 0;
	};
	std::vector<Manifest> manifests;
	std::unordered_set<std::string> filenames;
	auto query = database.prepare(R"sql(
SELECT a.artifact_id,a.filename,a.checksum_sha256,a.size_bytes,a.local_only,
 a.payload_present,COUNT(d.channel_id),
 COALESCE(SUM(CASE WHEN d.state=3 THEN 1 ELSE 0 END),0)
FROM artifacts a LEFT JOIN deliveries d ON d.artifact_id=a.artifact_id
WHERE a.delete_pending=0 GROUP BY a.artifact_id
)sql");
	while (query.step()) {
		manifests.push_back({query.text(0), query.text(1), query.text(2),
			static_cast<std::uint64_t>(query.integer(3)), query.integer(4) != 0,
			query.integer(5) != 0,
			static_cast<std::uint64_t>(query.integer(6)),
			static_cast<std::uint64_t>(query.integer(7))});
		filenames.insert(query.text(1));
	}
	for (const auto &manifest : manifests) {
		const auto path = payload_path(root, manifest.local_only,
			manifest.filename);
		if (!std::filesystem::is_regular_file(path)) {
			if (!manifest.local_only && manifest.delivery_count != 0 &&
			    manifest.delivery_count == manifest.succeeded_count) {
				auto update = database.prepare(
					"UPDATE artifacts SET payload_present=0,recovery_error='' "
					"WHERE artifact_id=?");
				update.bind(1, manifest.id);
				update.execute();
			} else if (manifest.payload_present) {
				auto update = database.prepare(
					"UPDATE artifacts SET payload_present=0,recovery_error=? "
					"WHERE artifact_id=?");
				update.bind(1, "manifest payload is missing");
				update.bind(2, manifest.id);
				update.execute();
				auto block = database.prepare(
					"UPDATE deliveries SET state=?,last_error=? WHERE artifact_id=? "
					"AND state NOT IN (?,?)");
				block.bind(1, integer(DeliveryState::Blocked));
				block.bind(2, "manifest payload is missing");
				block.bind(3, manifest.id);
				block.bind(4, integer(DeliveryState::Succeeded));
				block.bind(5,
					integer(DeliveryState::AdministrativelyDiscarded));
				block.execute();
			}
			continue;
		}
		std::error_code size_error;
		const auto actual_size = std::filesystem::file_size(path, size_error);
		if (size_error || actual_size != manifest.size) {
			block_artifact(manifest.id, "payload size mismatch");
			continue;
		}
		std::string body;
		try {
			body = read_file(path, manifest.size);
		} catch (const std::exception &) {
			block_artifact(manifest.id, "payload cannot be read");
			continue;
		}
		if (sha256(body) != manifest.checksum) {
			block_artifact(manifest.id, "payload checksum mismatch");
			continue;
		}
		if (!manifest.local_only && manifest.delivery_count != 0 &&
		    manifest.delivery_count == manifest.succeeded_count)
			finalize_completed_locked(manifest.id);
	}

	for (const auto &[directory, local_only] :
	     {std::pair{root / "outbox", false},
	      std::pair{root / "archive", true}}) {
		for (const auto &entry : std::filesystem::directory_iterator(directory)) {
			const auto filename = entry.path().filename().string();
			if (filename.contains(".tmp.")) {
				std::error_code ignored;
				std::filesystem::remove(entry.path(), ignored);
				continue;
			}
			if (!entry.is_regular_file() || !safe_filename(filename) ||
			    filenames.contains(filename))
				continue;
			const auto extension = extension_of(filename);
			if (extension != "json" && extension != "csv")
				continue;
			const auto size = entry.file_size();
			const auto body = read_file(entry.path(), size);
			const auto checksum = sha256(body);
			auto id = filename.substr(0, filename.size() - extension.size() - 1);
			if (!safe_identity(id))
				id = "recovered-" + checksum.substr(0, 24);
			auto collision = database.prepare(
				"SELECT 1 FROM artifacts WHERE artifact_id=?");
			collision.bind(1, id);
			if (collision.step())
				id += "-" + checksum.substr(0, 12);
			auto insert = database.prepare(R"sql(
INSERT INTO artifacts(artifact_id,job_id,job_revision,filename,mime_type,
 content_format,checksum_sha256,size_bytes,source_start_ns,source_end_ns,
 generated_at_ns,created_at_ns,local_only,payload_present,job_snapshot_json,
 recovery_error)
VALUES(?,?,?,?,?,?,?,?,0,0,0,0,?,1,'{}',?)
)sql");
			insert.bind(1, id);
			insert.bind(2, "recovered");
			insert.bind(3, std::uint64_t{1});
			insert.bind(4, filename);
			insert.bind(5, extension == "json" ? "application/json" :
				"text/csv; charset=utf-8");
			insert.bind(6, extension);
			insert.bind(7, checksum);
			insert.bind(8, size);
			insert.bind(9, static_cast<std::int32_t>(local_only ? 1 : 0));
			insert.bind(10, local_only
				? "recovered local-only file without manifest"
				: "recovered unsent file without delivery manifest");
			insert.execute();
		}
	}
}

} // namespace mnc::datalogger
