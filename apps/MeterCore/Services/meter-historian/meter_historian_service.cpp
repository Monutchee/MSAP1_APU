#include "meter_historian_service.hpp"

#include "msap1/settings/settings_ipc.hpp"
#include "mnc/MeterDataProvider/attributes/meter_attribute_set.hpp"

#include <boost/asio/post.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string_view>

#include <sys/stat.h>

namespace msap1::history::daemon {
namespace {

using mnc::ipc::ByteReader;
using mnc::ipc::ByteWriter;

constexpr std::array<MeasurementPeriod, 5> supported_periods = {
	MeasurementPeriod::Basic,
	MeasurementPeriod::Cycles150_180,
	MeasurementPeriod::Min10,
	MeasurementPeriod::Hour2,
	MeasurementPeriod::Demand,
};

const std::vector<mnc::meter::MeterAttributeId> supported_attributes = [] {
	using Id = mnc::meter::MeterAttributeId;
	std::vector<Id> result{Id::Frequency, Id::VanRms, Id::VbnRms,
		Id::VcnRms, Id::IaRms, Id::IbRms, Id::IcRms, Id::InRms};
	for (const auto group : {mnc::meter::MeterAttributeGroup::Energy,
		mnc::meter::MeterAttributeGroup::Demand})
		for (const auto key : mnc::meter::attributes_in(group))
			result.push_back(key.id);
	return result;
}();

mnc::ipc::Frame reply(const mnc::ipc::Frame &request, ByteWriter writer,
	mnc::ipc::FrameKind kind = mnc::ipc::FrameKind::response)
{
	return {.kind = kind, .message_type = request.message_type,
		.correlation_id = request.correlation_id,
		.payload = writer.take()};
}

void write_error(ByteWriter &writer, std::string_view message)
{
	writer.u32(1);
	writer.u32(static_cast<std::uint32_t>(message.size()));
	writer.bytes(mnc::ipc::to_payload(message));
}

void write_policy(ByteWriter &writer,
	const mnc::meter_stream::DatabaseStoragePolicy &policy)
{
	writer.u8(static_cast<std::uint8_t>(policy.dataset));
	writer.u8(static_cast<std::uint8_t>(policy.backend));
	writer.u16(0);
	writer.u64(policy.retention.maximum_age
		? static_cast<std::uint64_t>(
			policy.retention.maximum_age->count()) : 0);
	writer.u64(policy.retention.maximum_bytes.value_or(0));
}

mnc::meter_stream::DatabaseStoragePolicy read_policy(ByteReader &reader)
{
	mnc::meter_stream::DatabaseStoragePolicy policy;
	policy.dataset = static_cast<mnc::meter_stream::DatabaseDataset>(
		reader.u8());
	policy.backend = static_cast<mnc::meter_stream::StorageBackend>(
		reader.u8());
	(void)reader.u16();
	if (const auto age = reader.u64(); age != 0)
		policy.retention.maximum_age = std::chrono::seconds(age);
	if (const auto bytes = reader.u64(); bytes != 0)
		policy.retention.maximum_bytes = bytes;
	return policy;
}

} // namespace

MeterHistorianService::MeterHistorianService()
	: Service("MSAP1 meter historian", "meter-historian"),
	  server_(context_.get_executor(),
		std::string(msap1::history::ipc::socket_path),
		msap1::history::ipc::connection_limits)
{
}

void MeterHistorianService::on_start()
{
	const auto settings = msap1::settings::ipc::SettingsClient{}.active();
	store_ = std::make_unique<MeterHistoryStore>(
		"/data/mnc/database/meter-historian/historian.sqlite3",
		settings.database.historian_policies());
	stream_.register_consumer("historian");

	/* Volatile projections disappear with the process and are rebuilt from
	 * an independent spool cursor.  This deliberately runs in the consumer
	 * thread after the IPC server is ready: a large retained spool must never
	 * make systemd kill the historian before it can create its socket. */
	backfilling_ = std::ranges::any_of(store_->policies(), [](const auto &policy) {
		return policy.dataset !=
			mnc::meter_stream::DatabaseDataset::raw_record_spool &&
			policy.backend == mnc::meter_stream::StorageBackend::memory;
	});
	if (backfilling_) {
		(void)logger().write(mnc::logging::Priority::notice,
			"volatile historian rebuild will continue after service readiness",
			"volatile_history_rebuild_started");
	}

	server_.start(
		[this](auto connection, auto frame) {
			handle(std::move(connection), std::move(frame));
		},
		[this](const std::string &message) {
			(void)logger().write(mnc::logging::Priority::warning,
				message, "ipc_error");
		});
	if (::chmod(msap1::history::ipc::socket_path.data(), 0660) != 0)
		throw std::runtime_error("cannot set historian socket mode");

	io_worker_ = std::thread([this] {
		try {
			context_.run();
		} catch (...) {
			failed_ = true;
			request_stop();
		}
	});
	consumer_ = std::thread([this] { consume(); });
}

void MeterHistorianService::consume()
{
	if (backfilling_) {
		try {
			backfill();
			(void)logger().write(mnc::logging::Priority::notice,
				"volatile historian rebuild completed", "volatile_history_rebuild_completed");
		} catch (const std::exception &error) {
			consumer_healthy_ = false;
			(void)logger().write(mnc::logging::Priority::error,
				std::string("volatile historian rebuild failed: ") + error.what(),
				"volatile_history_rebuild_failed");
		}
		backfilling_ = false;
	}

	std::optional<BackfillSession> policy_backfill;
	while (!stopping_) {
		bool worked = false;
		auto idle_delay = std::chrono::milliseconds(100);
		try {
			auto records = stream_.read_after("historian", 256);
			if (!records.empty()) {
				/* Serialize only this bounded commit page with the routing
				 * switch. A potentially long replay runs cooperatively below
				 * and never holds live ingestion behind it. */
				{
					std::scoped_lock migration(migration_mutex_);
					for (const auto &record : records) {
						/* A skipped record still advances the cursor via
						 * the acknowledgement below; only a committed one
						 * is announced to subscribers. */
						if (ingest(record))
							post_event(ipc::Event::record_committed,
								   record.cursor);
					}
				}
				/* The per-record database commit is the durability boundary. A
				 * single page acknowledgement avoids one IPC round trip per
				 * record while retaining at-least-once replay. */
				stream_.acknowledge("historian", records.back().cursor);
				worked = true;
			}
			consumer_healthy_ = true;
		} catch (const std::exception &error) {
			consumer_healthy_ = false;
			(void)logger().write(mnc::logging::Priority::error,
				std::string("historian ingest failed: ") + error.what(),
				"historian_ingest_failed");
			/*
			 * A volatile spool loses the consumers table with the
			 * meter-stream process, and read_after() then rejects this
			 * consumer forever — re-registration is the only way back.
			 * It is idempotent (a live registration keeps its
			 * acknowledged cursor), so re-asserting it on every error
			 * needs no matching on the failure's cause.
			 */
			try {
				stream_.register_consumer("historian");
				(void)logger().write(mnc::logging::Priority::notice,
					"historian stream consumer re-registered",
					"historian_consumer_reregistered");
			} catch (const std::exception &) {
				/* The stream is unreachable; the retry below also
					 * covers this. */
				}
			idle_delay = std::chrono::seconds(1);
		}

		worked = service_policy_backfill(policy_backfill) || worked;
		if (!worked)
			std::this_thread::sleep_for(idle_delay);
	}
	if (policy_backfill)
		end_backfill(*policy_backfill);
}

bool MeterHistorianService::rebuilds_volatile_period(
	const mnc::meter_stream::MeterStreamRecord &record) const
{
	using Dataset = mnc::meter_stream::DatabaseDataset;
	const auto dataset = [&]() -> std::optional<Dataset> {
		if (record.record_format == msap1::meter_harmonic_aggregate_format) {
			switch (static_cast<MeasurementPeriod>(
				record.measurement_period)) {
			case MeasurementPeriod::Cycles150_180:
				return Dataset::harmonic_cycles_150_180;
			case MeasurementPeriod::Min10:
				return Dataset::harmonic_minutes_10;
			case MeasurementPeriod::Hour2:
				return Dataset::harmonic_hours_2;
			case MeasurementPeriod::Basic:
			case MeasurementPeriod::Min10Live:
			case MeasurementPeriod::Hour2Live:
			case MeasurementPeriod::Demand:
				return std::nullopt;
			}
		}
		switch (static_cast<MeasurementPeriod>(record.measurement_period)) {
		case MeasurementPeriod::Basic: return Dataset::basic;
		case MeasurementPeriod::Cycles150_180: return Dataset::cycles_150_180;
		case MeasurementPeriod::Min10: return Dataset::minutes_10;
		case MeasurementPeriod::Hour2: return Dataset::hours_2;
		case MeasurementPeriod::Demand: return Dataset::demand;
		case MeasurementPeriod::Min10Live:
		case MeasurementPeriod::Hour2Live:
			return std::nullopt;
		}
		return std::nullopt;
	}();
	if (!dataset)
		return false;
	return std::ranges::any_of(store_->policies(), [dataset](const auto &policy) {
		return policy.dataset == *dataset &&
			policy.backend == mnc::meter_stream::StorageBackend::memory;
	});
}

bool MeterHistorianService::ingest(
	const mnc::meter_stream::MeterStreamRecord &envelope)
{
	/* ENERGY is an atomic two-record family already committed by meter-stream.
	 * Its durable ledger view is sampled into history at the UTC boundary. */
	if (envelope.record_format == msap1::meter_energy_format)
		return false;
	/* Base HARMONIC-v1 remains the high-rate diagnostic/fallback family. R5C1
	 * emits the interval families used by history, so retaining base chunks
	 * would reproduce the write amplification this offload removes. */
	if (envelope.record_format == msap1::meter_harmonic_format)
		return false;
	if (envelope.record_format == msap1::meter_harmonic_aggregate_format) {
		try {
			if (envelope.payload.size() != sizeof(msap1::MeterRecord))
				throw std::invalid_argument(
					"historian harmonic record size mismatch");
			msap1::MeterRecord raw{};
			std::memcpy(&raw, envelope.payload.data(), sizeof(raw));
			return store_->append_harmonic_record(raw, envelope.cursor,
				envelope.timing.utc_start_nanoseconds.value_or(
					envelope.ingested_at_nanoseconds));
		} catch (const std::invalid_argument &error) {
			const auto skipped = ++undecodable_records_;
			if (skipped == 1 || skipped % 100 == 0)
				(void)logger().write(mnc::logging::Priority::warning,
					"skipped an undecodable harmonic record at cursor " +
						std::to_string(envelope.cursor) + ": " +
						error.what(), "historian_record_skipped");
			return false;
		}
	}
	/*
	 * A record that cannot be decoded is a POISON PILL, and the spool is
	 * durable: the record survives every restart, so retrying it wedges the
	 * consumer permanently and no history is ever rebuilt. One malformed
	 * record must cost exactly one record.
	 *
	 * This is not hypothetical. Records captured before the PL provenance
	 * fix carry a zero first-sample index, which the decoder now correctly
	 * rejects; replaying that spool previously restarted this service in a
	 * loop and left the whole History page unavailable.
	 *
	 * The distinction that matters: decode failures describe the RECORD and
	 * are skipped, while a store_->append() failure is systemic (database,
	 * disk, storage policy) and must propagate so the batch is retried
	 * instead of silently dropping good measurements.
	 */
	msap1::MeterUpdate update;
	try {
		if (envelope.payload.size() != sizeof(msap1::MeterRecord))
			throw std::invalid_argument(
				"historian record size mismatch");
		msap1::MeterRecord raw{};
		std::memcpy(&raw, envelope.payload.data(), sizeof(raw));
		update = msap1::MeterDecoderRegistry::with_builtin_decoders()
			.decode(raw, std::chrono::system_clock::time_point(
				std::chrono::nanoseconds(
					envelope.ingested_at_nanoseconds)));
	} catch (const std::invalid_argument &error) {
		const auto skipped = ++undecodable_records_;
		/* Rate limited: a spool retained across a long fault window can
		 * hold thousands of these, and flooding the journal would bury
		 * the cause. The counter is the complete tally. */
		if (skipped == 1 || skipped % 100 == 0)
			(void)logger().write(mnc::logging::Priority::warning,
				"skipped an undecodable spooled record at cursor " +
					std::to_string(envelope.cursor) + ": " +
					error.what() + " (" +
					std::to_string(skipped) +
					" skipped so far)",
				"historian_record_skipped");
		return false;
	}
	/* M15 open-window records are explicitly non-normative operational
	 * previews. They share the durable transport with completed records so
	 * all consumers see one ordered contract, but they must never enter the
	 * compliance historian. consume() still acknowledges this cursor. */
	if (update.period == MeasurementPeriod::Min10Live ||
	    update.period == MeasurementPeriod::Hour2Live)
		return false;
	if (envelope.record_format == msap1::meter_demand_format) {
		/* DEMAND-v1 is durable at its live cadence, but persistent history stays
		 * at one point per UTC ten-minute boundary. Fixed-block records already
		 * are such a boundary; sliding records are sampled below when the final
		 * ten-minute sibling arrives. */
		update.demand = stream_.demand();
		if (!update.demand)
			throw std::runtime_error(
				"durable DEMAND checkpoint is missing at history boundary");
		if (update.demand->method == msap1::DemandMethod::sliding)
			return false;
	}
	if (update.period == MeasurementPeriod::Min10 &&
	    update.kind == RecordKind::fundamental) {
		/* ENERGY records precede the aggregate family in the authoritative
		 * stream, so its durable cumulative checkpoint is coherent here. */
		update.energy = stream_.energy();
	}
	if (update.period == MeasurementPeriod::Min10 &&
	    update.kind == RecordKind::unbalance) {
		/* The 150/180-cycle sliding DEMAND record precedes a coincident
		 * ten-minute family. Reuse this final sibling's unique stream cursor for
		 * the dedicated demand dataset, avoiding a three-second forever log. */
		if (auto demand = stream_.demand(); demand &&
		    demand->method == msap1::DemandMethod::sliding) {
			update.period = MeasurementPeriod::Demand;
			update.kind = RecordKind::demand;
			update.demand = std::move(demand);
		}
	}
	store_->append(update, envelope.cursor,
		envelope.timing.utc_start_nanoseconds.value_or(
			envelope.ingested_at_nanoseconds));
	return true;
}

void MeterHistorianService::backfill()
{
	auto session = begin_backfill(0);
	try {
		while (!stopping_ &&
		       backfill_page(session, false) ==
			       BackfillPageResult::progress) {
		}
		end_backfill(session);
	} catch (...) {
		end_backfill(session);
		throw;
	}
}

MeterHistorianService::BackfillSession
MeterHistorianService::begin_backfill(std::uint64_t generation)
{
	constexpr std::string_view consumer = "historian-policy-backfill";
	BackfillSession session;
	session.stream = std::make_unique<
		msap1::meter_stream::MeterRecordStreamClient>();
	session.generation = generation;
	session.stream->unregister_consumer(consumer);
	session.stream->register_consumer(consumer);
	const auto stream_status = session.stream->status();
	session.through_cursor = stream_status.newest_cursor;
	oldest_available_stream_cursor_ = stream_status.oldest_cursor;
	backfill_incomplete_ = backfill_is_incomplete(stream_status.oldest_cursor,
		stream_status.session_start_cursor,
		store_->persisted_stream_high_water());
	if (backfill_incomplete_) {
		(void)logger().write(mnc::logging::Priority::warning,
			"historian backfill begins after records already pruned from the spool",
			"historian_backfill_incomplete");
	}
	return session;
}

MeterHistorianService::BackfillPageResult
MeterHistorianService::backfill_page(BackfillSession &session,
	bool enforce_generation)
{
	constexpr std::string_view consumer = "historian-policy-backfill";
	if (session.through_cursor == 0)
		return BackfillPageResult::complete;

	/* Sixty-four records bounds the replay lock hold while still carrying at
	 * least one complete 42-record harmonic family. The live page is always
	 * serviced before the next replay page. */
	auto records = session.stream->read_after(consumer, 64);
	if (records.empty())
		return BackfillPageResult::complete;

	std::uint64_t acknowledged = 0;
	{
		std::scoped_lock migration(migration_mutex_);
		if (enforce_generation &&
		    (session.generation != policy_backfill_generation_ ||
		     !policy_backfill_requested_))
			return BackfillPageResult::cancelled;
		for (const auto &record : records) {
			if (record.cursor > session.through_cursor)
				break;
			/* Persistent projections already retain their committed rows.
			 * Replaying only selected volatile datasets avoids rewriting
			 * every persistent record during a process restart. */
			if (rebuilds_volatile_period(record))
				(void)ingest(record);
			acknowledged = record.cursor;
		}
	}

	if (acknowledged != 0)
		session.stream->acknowledge(consumer, acknowledged);
	return acknowledged == 0 || acknowledged >= session.through_cursor
		? BackfillPageResult::complete
		: BackfillPageResult::progress;
}

void MeterHistorianService::end_backfill(BackfillSession &session) noexcept
{
	constexpr std::string_view consumer = "historian-policy-backfill";
	try {
		if (session.stream)
			session.stream->unregister_consumer(consumer);
	} catch (...) {
	}
	session.stream.reset();
}

bool MeterHistorianService::service_policy_backfill(
	std::optional<BackfillSession> &session)
{
	std::uint64_t requested_generation = 0;
	{
		std::scoped_lock migration(migration_mutex_);
		if (policy_backfill_requested_)
			requested_generation = policy_backfill_generation_;
	}

	if (session && (requested_generation == 0 ||
		       session->generation != requested_generation)) {
		end_backfill(*session);
		session.reset();
	}
	if (requested_generation == 0)
		return false;

	try {
		if (!session)
			session.emplace(begin_backfill(requested_generation));
		const auto result = backfill_page(*session, true);
		if (result == BackfillPageResult::progress)
			return true;

		end_backfill(*session);
		session.reset();
		if (result == BackfillPageResult::cancelled)
			return true;

		bool completed = false;
		{
			std::scoped_lock migration(migration_mutex_);
			if (policy_backfill_requested_ &&
			    policy_backfill_generation_ == requested_generation) {
				policy_backfill_requested_ = false;
				migrating_ = false;
				completed = true;
			}
		}
		if (completed)
			post_event(ipc::Event::migration_completed);
	} catch (const std::exception &error) {
		if (session) {
			end_backfill(*session);
			session.reset();
		}
		bool failed = false;
		{
			std::scoped_lock migration(migration_mutex_);
			if (policy_backfill_requested_ &&
			    policy_backfill_generation_ == requested_generation) {
				policy_backfill_requested_ = false;
				migrating_ = false;
				backfill_incomplete_ = true;
				failed = true;
			}
		}
		if (failed) {
			post_event(ipc::Event::migration_failed);
			(void)logger().write(mnc::logging::Priority::error,
				std::string("historian policy backfill failed: ") +
					error.what(),
				"historian_policy_backfill_failed");
		}
	}
	return true;
}

void MeterHistorianService::on_stop() noexcept
{
	stopping_ = true;
	{
		std::scoped_lock migration(migration_mutex_);
		++policy_backfill_generation_;
		policy_backfill_requested_ = false;
		migrating_ = false;
	}
	server_.stop();
	context_.stop();
	if (consumer_.joinable())
		consumer_.join();
	if (io_worker_.joinable())
		io_worker_.join();
	store_.reset();
}

void MeterHistorianService::on_reload()
{
	(void)logger().write(mnc::logging::Priority::notice,
		"historian reload requested; settings are applied transactionally over IPC",
		"reload_requested");
}

mnc::ServiceHealth MeterHistorianService::health() const
{
	if (failed_)
		return {false, "historian IPC worker failed"};
	if (!consumer_healthy_)
		return {false, "historian stream consumer is retrying"};
	/* Skipped records are not a failure -- the service is serving history --
	 * but they are data loss and must never be silent. */
	const auto skipped = undecodable_records_.load();
	const auto suffix = skipped == 0
		? std::string{}
		: "; " + std::to_string(skipped) +
			  " undecodable spooled record(s) skipped";
	if (backfilling_)
		return {true, "historian ready; volatile history rebuild in progress" +
				      suffix};
	return {true, "historian ready" + suffix};
}

void MeterHistorianService::handle(
	mnc::ipc::UnixStreamServer::Connection connection, mnc::ipc::Frame frame)
{
	ByteWriter output;
	try {
		if (frame.kind != mnc::ipc::FrameKind::request)
			throw std::invalid_argument("historian frame is not a request");
		ByteReader input(frame.payload);
		switch (static_cast<ipc::Command>(frame.message_type)) {
		case ipc::Command::query_history: {
			const auto query = ipc::decode_query(input);
			input.require_finished();
			const auto points = store_->query(query);
			output.u32(0);
			output.u32(static_cast<std::uint32_t>(points.size()));
			for (const auto &point : points) {
				output.i64(point.measured_at_nanoseconds);
				output.u64(point.source_sequence);
				output.u16(static_cast<std::uint16_t>(point.attribute));
				output.u8(static_cast<std::uint8_t>(point.quality));
				output.u8(point.reset_epoch.has_value());
				output.i64(point.value);
				output.u64(point.reset_epoch.value_or(0));
			}
			break;
		}
		case ipc::Command::get_historian_status: {
			input.require_finished();
			auto status = store_->status();
			status.healthy = health().healthy;
			status.migration_in_progress = migrating_;
			status.backfill_incomplete = backfill_incomplete_;
			status.oldest_available_stream_cursor =
				oldest_available_stream_cursor_;
			output.u32(0);
			output.u8(status.healthy);
			output.u8(status.migration_in_progress);
			output.u8(status.backfill_incomplete);
			output.u8(0);
			output.u64(status.acknowledged_cursor);
			output.u64(status.oldest_available_stream_cursor);
			output.u64(status.block_count);
			output.u64(status.storage_bytes);
			output.u32(static_cast<std::uint32_t>(
				status.datasets.size()));
			for (const auto &item : status.datasets) {
				output.u8(static_cast<std::uint8_t>(item.dataset));
				output.u8(static_cast<std::uint8_t>(item.backend));
				output.u8(item.oldest_nanoseconds.has_value());
				output.u8(item.newest_nanoseconds.has_value());
				output.u64(item.block_count);
				output.u64(item.storage_bytes);
				output.i64(item.oldest_nanoseconds.value_or(0));
				output.i64(item.newest_nanoseconds.value_or(0));
			}
			break;
		}
		case ipc::Command::get_capabilities:
			input.require_finished();
			output.u32(0);
			output.u32(static_cast<std::uint32_t>(
				supported_periods.size()));
			for (const auto period : supported_periods)
				output.u8(static_cast<std::uint8_t>(period));
			output.u32(static_cast<std::uint32_t>(
				supported_attributes.size()));
			for (const auto attribute : supported_attributes)
				output.u16(static_cast<std::uint16_t>(attribute));
			output.u32(50000);
			break;
		case ipc::Command::get_storage_policy: {
			input.require_finished();
			const auto policies = store_->policies();
			output.u32(0);
			output.u32(static_cast<std::uint32_t>(policies.size()));
			for (const auto &policy : policies)
				write_policy(output, policy);
			break;
		}
		case ipc::Command::apply_storage_policy: {
			const auto count = input.u32();
			std::vector<mnc::meter_stream::DatabaseStoragePolicy> policies;
			policies.reserve(count);
			for (std::uint32_t index = 0; index < count; ++index)
				policies.push_back(read_policy(input));
			input.require_finished();
			const auto old = store_->policies();
			if (mnc::meter_stream::same_database_policies(old, policies)) {
				output.u32(0);
				break;
			}
			const bool requires_backfill =
				historian_policy_transition_requires_backfill(
					old, policies);
			migrating_ = true;
			post_event(ipc::Event::migration_started);
			try {
				{
					std::scoped_lock migration(migration_mutex_);
					store_->prepare_policy_migration(policies);
					store_->apply_policies(policies);
					++policy_backfill_generation_;
					policy_backfill_requested_ = requires_backfill;
				}
				if (!requires_backfill) {
					migrating_ = false;
					post_event(ipc::Event::migration_completed);
				}
			} catch (...) {
				{
					std::scoped_lock migration(migration_mutex_);
					store_->apply_policies(old);
					++policy_backfill_generation_;
					policy_backfill_requested_ = false;
				}
				migrating_ = false;
				post_event(ipc::Event::migration_failed);
				throw;
			}
			output.u32(0);
			break;
		}
		case ipc::Command::subscribe_historian_events:
			input.require_finished();
			subscribe(connection);
			output.u32(0);
			break;
		case ipc::Command::clear_datasets: {
			const auto count = input.u32();
			if (count == 0 || count > supported_periods.size())
				throw std::invalid_argument(
					"historian clear requires one to four datasets");
			std::vector<mnc::meter_stream::DatabaseDataset> datasets;
			datasets.reserve(count);
			for (std::uint32_t index = 0; index < count; ++index)
				datasets.push_back(
					static_cast<mnc::meter_stream::DatabaseDataset>(
						input.u8()));
			input.require_finished();

			std::scoped_lock maintenance(migration_mutex_);
			migrating_ = true;
			post_event(ipc::Event::maintenance_started);
			try {
				const auto cursor = stream_.status().newest_cursor;
				store_->clear_datasets(datasets, cursor);
				migrating_ = false;
				post_event(ipc::Event::maintenance_completed, cursor);
				(void)logger().write(mnc::logging::Priority::notice,
					"selected historian datasets cleared",
					"historian_datasets_cleared");
			} catch (...) {
				migrating_ = false;
				post_event(ipc::Event::maintenance_failed);
				throw;
			}
			output.u32(0);
			break;
		}
		case ipc::Command::recreate_database: {
			input.require_finished();
			std::scoped_lock maintenance(migration_mutex_);
			migrating_ = true;
			post_event(ipc::Event::maintenance_started);
			try {
				const auto cursor = stream_.status().newest_cursor;
				store_->recreate_database(cursor);
				migrating_ = false;
				post_event(ipc::Event::maintenance_completed, cursor);
				(void)logger().write(mnc::logging::Priority::warning,
					"historian database deleted and recreated",
					"historian_database_recreated");
			} catch (...) {
				migrating_ = false;
				post_event(ipc::Event::maintenance_failed);
				throw;
			}
			output.u32(0);
			break;
		}
		default:
			throw std::invalid_argument("unsupported historian command");
		}
		connection->post_send(reply(frame, std::move(output)));
	} catch (const std::exception &error) {
		ByteWriter failure;
		write_error(failure, error.what());
		connection->post_send(reply(frame, std::move(failure),
			mnc::ipc::FrameKind::error));
	}
}

void MeterHistorianService::subscribe(
	const mnc::ipc::UnixStreamServer::Connection &connection)
{
	subscribers_.erase(std::remove_if(subscribers_.begin(), subscribers_.end(),
		[](const auto &candidate) { return candidate.expired(); }),
		subscribers_.end());
	subscribers_.push_back(connection);
}

void MeterHistorianService::post_event(ipc::Event event,
	std::uint64_t cursor)
{
	boost::asio::post(context_, [this, event, cursor] {
		publish_event(event, cursor);
	});
}

void MeterHistorianService::publish_event(ipc::Event event,
	std::uint64_t cursor)
{
	ByteWriter payload;
	payload.u32(static_cast<std::uint32_t>(event));
	payload.u64(cursor);
	for (auto iterator = subscribers_.begin();
		iterator != subscribers_.end();) {
		if (auto connection = iterator->lock();
			connection && connection->is_open()) {
			connection->post_send({.kind = mnc::ipc::FrameKind::event,
				.message_type = static_cast<std::uint32_t>(
					ipc::Command::subscribe_historian_events),
				.correlation_id = 0,
				.payload = payload.data()});
			++iterator;
		} else {
			iterator = subscribers_.erase(iterator);
		}
	}
}

} // namespace msap1::history::daemon
