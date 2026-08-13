#pragma once

#include "mnc/MeterDataProvider/snapshot/meter_snapshot_provider.hpp"
#include "mnc/mqtt/mqtt_client.hpp"
#include "msap1/mqtt/meter_snapshot_payload_encoder.hpp"
#include "msap1/settings/definition/mqtt_settings.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/steady_timer.hpp>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace msap1::mqtt {

struct PublicationStatistics {
	std::uint64_t attempts = 0;
	std::uint64_t successes = 0;
	std::uint64_t failures = 0;
	std::uint64_t last_source_sequence = 0;
	std::int64_t last_successful_publish_unix_ms = 0;
	std::string last_error;
};

/**
 * Independent latest-state publication timers.
 *
 * No record queue is created here: one pending request per publication ID is
 * replaced while the broker is unavailable, which is the intended lossy
 * snapshot contract. Durable consumers belong to MeterRecordStream.
 */
class MeterPublicationScheduler {
public:
	MeterPublicationScheduler(boost::asio::any_io_executor executor,
		mnc::meter::MeterSnapshotProvider &provider,
		mnc::mqtt::MqttClient &client);
	~MeterPublicationScheduler();

	void configure(std::vector<msap1::settings::MqttPublicationSettings> settings);
	void start();
	void stop() noexcept;
	/** Publish the newest payload retained for each publication. */
	void flush_pending();

	[[nodiscard]] std::map<std::string, PublicationStatistics> statistics() const;

private:
	struct Publication;
	void schedule(const std::shared_ptr<Publication> &publication);
	void publish_once(const std::shared_ptr<Publication> &publication);

	boost::asio::any_io_executor executor_;
	mnc::meter::MeterSnapshotProvider &provider_;
	mnc::mqtt::MqttClient &client_;
	MeterSnapshotPayloadEncoder encoder_;
	mutable std::mutex mutex_;
	std::vector<std::shared_ptr<Publication>> publications_;
	std::map<std::string, mnc::mqtt::PublishRequest> pending_;
	bool running_ = false;
};

} // namespace msap1::mqtt
