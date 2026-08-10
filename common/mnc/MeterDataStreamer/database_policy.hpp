#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mnc::meter_stream {

enum class StorageBackend : std::uint8_t { memory = 0, persistent = 1 };

enum class DatabaseDataset : std::uint8_t {
	raw_record_spool = 0,
	basic,
	cycles_150_180,
	minutes_10,
	hours_2,
};

struct RetentionPolicy {
	/** Null means forever. */
	std::optional<std::chrono::seconds> maximum_age;
	/** Null means no explicit byte cap. */
	std::optional<std::uint64_t> maximum_bytes;
};

struct DatabaseStoragePolicy {
	DatabaseDataset dataset = DatabaseDataset::raw_record_spool;
	StorageBackend backend = StorageBackend::persistent;
	RetentionPolicy retention{};
};

/** Reject malformed policies before they can select a database or retention path. */
inline void validate_database_policy(const DatabaseStoragePolicy &policy)
{
	switch (policy.dataset) {
	case DatabaseDataset::raw_record_spool:
	case DatabaseDataset::basic:
	case DatabaseDataset::cycles_150_180:
	case DatabaseDataset::minutes_10:
	case DatabaseDataset::hours_2:
		break;
	default:
		throw std::invalid_argument("unknown database dataset");
	}

	switch (policy.backend) {
	case StorageBackend::memory:
	case StorageBackend::persistent:
		break;
	default:
		throw std::invalid_argument("unknown database storage backend");
	}

	if (policy.retention.maximum_age &&
	    policy.retention.maximum_age->count() <= 0)
		throw std::invalid_argument("database retention age must be positive");
	if (policy.retention.maximum_bytes &&
	    *policy.retention.maximum_bytes == 0)
		throw std::invalid_argument("database retention byte limit must be positive");
}

[[nodiscard]] constexpr std::string_view dataset_name(DatabaseDataset dataset)
{
	switch (dataset) {
	case DatabaseDataset::raw_record_spool: return "spool";
	case DatabaseDataset::basic: return "basic";
	case DatabaseDataset::cycles_150_180: return "cycles_150_180";
	case DatabaseDataset::minutes_10: return "minutes_10";
	case DatabaseDataset::hours_2: return "hours_2";
	}
	return "unknown";
}

/** Thread-safe runtime policy catalog shared by services and status APIs. */
class DatabasePolicyManager final {
public:
	explicit DatabasePolicyManager(std::vector<DatabaseStoragePolicy> policies)
	{
		apply(std::move(policies));
	}

	void apply(std::vector<DatabaseStoragePolicy> policies)
	{
		std::map<DatabaseDataset, DatabaseStoragePolicy> staged;
		for (auto &policy : policies) {
			validate_database_policy(policy);
			if (!staged.emplace(policy.dataset, policy).second)
				throw std::invalid_argument("duplicate database policy");
		}
		std::scoped_lock lock(mutex_);
		policies_ = std::move(staged);
	}

	[[nodiscard]] DatabaseStoragePolicy policy(DatabaseDataset dataset) const
	{
		std::scoped_lock lock(mutex_);
		const auto found = policies_.find(dataset);
		if (found == policies_.end())
			throw std::invalid_argument("database policy is not configured");
		return found->second;
	}

	[[nodiscard]] std::vector<DatabaseStoragePolicy> policies() const
	{
		std::scoped_lock lock(mutex_);
		std::vector<DatabaseStoragePolicy> result;
		for (const auto &[dataset, policy] : policies_) {
			(void)dataset;
			result.push_back(policy);
		}
		return result;
	}

private:
	mutable std::mutex mutex_;
	std::map<DatabaseDataset, DatabaseStoragePolicy> policies_;
};

} // namespace mnc::meter_stream
