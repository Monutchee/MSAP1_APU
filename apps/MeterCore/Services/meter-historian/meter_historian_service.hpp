#pragma once

#include "mnc/ipc/ipc.hpp"
#include "mnc/service/service.hpp"
#include "msap1/meter/history/historian_ipc.hpp"
#include "msap1/meter/history/meter_history.hpp"
#include "msap1/meter/MeterDataProvider/stream/meter_stream_ipc.hpp"

#include <boost/asio/io_context.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace msap1::history::daemon {

/**
 * Durable stream consumer that materializes period-specific historical
 * projections. The stream cursor is acknowledged only after the decoded
 * block and all of its values commit successfully.
 */
class MeterHistorianService final : public mnc::Service {
public:
	MeterHistorianService();

protected:
	void on_start() override;
	void on_reload() override;
	void on_stop() noexcept override;
	[[nodiscard]] mnc::ServiceHealth health() const override;

private:
	struct BackfillSession {
		std::unique_ptr<msap1::meter_stream::MeterRecordStreamClient>
			stream;
		std::uint64_t through_cursor = 0;
		std::uint64_t generation = 0;
	};
	enum class BackfillPageResult { progress, complete, cancelled };

	void consume();
	/**
	 * Project one spooled record. Returns false when the record itself is
	 * undecodable and was skipped; throws only on systemic failures (a
	 * database or storage error) so those are still retried.
	 */
	bool ingest(const mnc::meter_stream::MeterStreamRecord &record);
	void backfill();
	[[nodiscard]] BackfillSession begin_backfill(
		std::uint64_t generation);
	[[nodiscard]] BackfillPageResult backfill_page(
		BackfillSession &session, bool enforce_generation);
	void end_backfill(BackfillSession &session) noexcept;
	[[nodiscard]] bool service_policy_backfill(
		std::optional<BackfillSession> &session);
	[[nodiscard]] bool rebuilds_volatile_period(
		const mnc::meter_stream::MeterStreamRecord &record) const;
	void handle(mnc::ipc::UnixStreamServer::Connection connection,
		mnc::ipc::Frame frame);
	void subscribe(const mnc::ipc::UnixStreamServer::Connection &connection);
	void post_event(ipc::Event event, std::uint64_t cursor = 0);
	void publish_event(ipc::Event event, std::uint64_t cursor);

	boost::asio::io_context context_;
	msap1::meter_stream::MeterRecordStreamClient stream_;
	std::unique_ptr<MeterHistoryStore> store_;
	mnc::ipc::UnixStreamServer server_;
	std::thread io_worker_;
	std::thread consumer_;
	std::atomic<bool> stopping_{false};
	std::atomic<bool> failed_{false};
	std::atomic<bool> consumer_healthy_{true};
	std::atomic<bool> migrating_{false};
	std::atomic<bool> backfilling_{false};
	std::atomic<bool> backfill_incomplete_{false};
	std::atomic<std::uint64_t> oldest_available_stream_cursor_{0};
	/* Spooled records skipped because they could not be decoded. The spool
	 * is durable, so such a record persists across restarts: it must cost
	 * one record, never the whole projection. */
	std::atomic<std::uint64_t> undecodable_records_{0};
	/* The live consumer and policy replay share one writer thread. A policy
	 * update only holds this mutex for the routing switch; the consumer then
	 * alternates live pages with bounded replay pages. */
	std::mutex migration_mutex_;
	std::uint64_t policy_backfill_generation_ = 0;
	bool policy_backfill_requested_ = false;
	std::vector<std::weak_ptr<mnc::ipc::FramedConnection>> subscribers_;
};

} // namespace msap1::history::daemon
