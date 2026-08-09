#pragma once

#include "mnc/MeterDataProvider/snapshot/meter_snapshot.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace mnc::meter {

class LatestSubscription {
public:
	LatestSubscription() = default;
	explicit LatestSubscription(std::shared_ptr<void> owner)
		: owner_(std::move(owner))
	{
	}

private:
	std::shared_ptr<void> owner_;
};

class MeterSnapshotProvider {
public:
	using Callback = std::function<void(const MeterSnapshot &)>;
	virtual ~MeterSnapshotProvider() = default;

	[[nodiscard]] virtual std::vector<MeterCapabilities>
	capabilities() const = 0;
	[[nodiscard]] virtual std::optional<MeterSnapshot>
	latest(const MeterSnapshotRequest &request) const = 0;
	[[nodiscard]] virtual LatestSubscription
	subscribe_latest(const MeterSnapshotRequest &request,
			 Callback callback) = 0;
};

} // namespace mnc::meter
