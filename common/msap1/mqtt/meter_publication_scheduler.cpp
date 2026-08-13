#include "msap1/mqtt/meter_publication_scheduler.hpp"

#include "msap1/mqtt/meter_publication_catalog.hpp"

#include <boost/asio/post.hpp>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace msap1::mqtt {

namespace {
std::int64_t unix_time_milliseconds()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
}
} // namespace

struct MeterPublicationScheduler::Publication {
	Publication(boost::asio::any_io_executor executor,
		msap1::settings::MqttPublicationSettings value)
		: settings(std::move(value)), timer(std::move(executor))
	{
	}

	msap1::settings::MqttPublicationSettings settings;
	boost::asio::steady_timer timer;
	std::vector<mnc::meter::MeterAttributeKey> attributes;
	PublicationStatistics statistics;
};

MeterPublicationScheduler::MeterPublicationScheduler(
	boost::asio::any_io_executor executor,
	mnc::meter::MeterSnapshotProvider &provider,
	mnc::mqtt::MqttClient &client)
	: executor_(std::move(executor)), provider_(provider), client_(client)
{
}

MeterPublicationScheduler::~MeterPublicationScheduler()
{
	stop();
}

void MeterPublicationScheduler::configure(
	std::vector<msap1::settings::MqttPublicationSettings> settings)
{
	stop();
	std::vector<std::shared_ptr<Publication>> publications;
	for (auto &configuration : settings) {
		if (!configuration.enabled)
			continue;
		auto publication = std::make_shared<Publication>(
			executor_, std::move(configuration));
		for (const auto &name : publication->settings.attributes) {
			const auto attribute = mnc::meter::find_attribute(name);
			if (!attribute)
				throw std::invalid_argument("unknown MQTT attribute " + name);
			const auto period = MeterPublicationCatalog::period(
				publication->settings.period);
			const auto capabilities = provider_.capabilities();
			const auto supported = std::ranges::find_if(capabilities,
				[period](const auto &candidate) {
					return candidate.period == period;
				});
			if (supported == capabilities.end() ||
			    std::ranges::find(supported->attributes, *attribute) ==
				supported->attributes.end())
				throw std::invalid_argument(
					"MQTT attribute is unavailable for selected period: " +
					name);
			publication->attributes.push_back(*attribute);
		}
		publications.push_back(std::move(publication));
	}
	std::scoped_lock lock(mutex_);
	publications_ = std::move(publications);
	pending_.clear();
}

void MeterPublicationScheduler::flush_pending()
{
	std::map<std::string, mnc::mqtt::PublishRequest> pending;
	{
		std::scoped_lock lock(mutex_);
		pending = pending_;
	}
	for (const auto &[id, request] : pending) {
		try {
			client_.publish(request);
			std::scoped_lock lock(mutex_);
			const auto current = pending_.find(id);
			if (current != pending_.end() &&
			    current->second.payload == request.payload)
				pending_.erase(current);
			const auto publication = std::ranges::find_if(publications_,
				[&id](const auto &candidate) {
					return candidate->settings.id == id;
				});
			if (publication != publications_.end()) {
				++(*publication)->statistics.successes;
				(*publication)->statistics.last_successful_publish_unix_ms =
					unix_time_milliseconds();
				(*publication)->statistics.last_error.clear();
			}
		} catch (const std::exception &error) {
			std::scoped_lock lock(mutex_);
			const auto publication = std::ranges::find_if(publications_,
				[&id](const auto &candidate) {
					return candidate->settings.id == id;
				});
			if (publication != publications_.end()) {
				++(*publication)->statistics.failures;
				(*publication)->statistics.last_error = error.what();
			}
		}
	}
}

void MeterPublicationScheduler::start()
{
	std::vector<std::shared_ptr<Publication>> publications;
	{
		std::scoped_lock lock(mutex_);
		if (running_)
			return;
		running_ = true;
		publications = publications_;
	}
	for (const auto &publication : publications)
		schedule(publication);
}

void MeterPublicationScheduler::stop() noexcept
{
	std::vector<std::shared_ptr<Publication>> publications;
	{
		std::scoped_lock lock(mutex_);
		running_ = false;
		publications = publications_;
	}
	for (const auto &publication : publications) {
		boost::system::error_code ignored;
		publication->timer.cancel(ignored);
	}
}

void MeterPublicationScheduler::schedule(
	const std::shared_ptr<Publication> &publication)
{
	publication->timer.expires_after(
		std::chrono::milliseconds(publication->settings.interval_ms));
	publication->timer.async_wait([this, publication](const auto &error) {
		if (error)
			return;
		{
			std::scoped_lock lock(mutex_);
			if (!running_)
				return;
		}
		publish_once(publication);
		schedule(publication);
	});
}

void MeterPublicationScheduler::publish_once(
	const std::shared_ptr<Publication> &publication)
{
	try {
		const auto period = MeterPublicationCatalog::period(
			publication->settings.period);
		mnc::meter::MeterSnapshotRequest request{
			.period = period, .attributes = publication->attributes};
		const auto snapshot = provider_.latest(request);
		if (!snapshot)
			throw std::runtime_error("meter snapshot is unavailable");
		mnc::mqtt::PublishRequest publish{
			.publication_id = publication->settings.id,
			.topic = publication->settings.topic,
			.payload = encoder_.encode(*snapshot,
				publication->settings.id, publication->attributes),
			.qos = publication->settings.qos,
			.retain = publication->settings.retain};

		{
			std::scoped_lock lock(mutex_);
			++publication->statistics.attempts;
			publication->statistics.last_source_sequence = snapshot->sequence;
			pending_[publication->settings.id] = publish;
		}

		// A successful publish removes exactly the payload just attempted. If a
		// later tick replaced it, leave the newer pending value intact.
		client_.publish(publish);
		std::scoped_lock lock(mutex_);
		++publication->statistics.successes;
		publication->statistics.last_successful_publish_unix_ms =
			unix_time_milliseconds();
		publication->statistics.last_error.clear();
		const auto pending = pending_.find(publication->settings.id);
		if (pending != pending_.end() && pending->second.payload == publish.payload)
			pending_.erase(pending);
	} catch (const std::exception &error) {
		std::scoped_lock lock(mutex_);
		++publication->statistics.failures;
		publication->statistics.last_error = error.what();
	}
}

std::map<std::string, PublicationStatistics>
MeterPublicationScheduler::statistics() const
{
	std::map<std::string, PublicationStatistics> result;
	std::scoped_lock lock(mutex_);
	for (const auto &publication : publications_)
		result.emplace(publication->settings.id, publication->statistics);
	return result;
}

} // namespace msap1::mqtt
