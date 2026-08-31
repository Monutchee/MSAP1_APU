#include "mnc/datalogger/scheduler.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <ranges>
#include <stdexcept>

namespace mnc::datalogger {
namespace {

constexpr UtcNanoseconds generation_settle_delay = 30'000'000'000ll;
constexpr UtcNanoseconds delivery_lease = 3'660'000'000'000ll;
constexpr UtcNanoseconds second = 1'000'000'000ll;

UtcNanoseconds next_boundary(UtcNanoseconds now, UtcNanoseconds interval)
{
	if (now < 0 || interval <= 0 ||
	    now > std::numeric_limits<UtcNanoseconds>::max() - interval)
		throw std::invalid_argument("UTC scheduling interval is invalid");
	return ((now + interval - 1) / interval) * interval;
}

std::uint64_t fnv1a(std::string_view value, std::uint32_t attempt) noexcept
{
	std::uint64_t result = 1469598103934665603ull;
	for (const unsigned char character : value) {
		result ^= character;
		result *= 1099511628211ull;
	}
	for (int shift = 0; shift < 32; shift += 8) {
		result ^= (attempt >> shift) & 0xffu;
		result *= 1099511628211ull;
	}
	return result;
}

} // namespace

DataSenderEngine::DataSenderEngine(const Datalogger &datalogger,
	const MeterDataContentWriterFactory &writers, OutboxRepository &outbox,
	const Clock &clock)
	: datalogger_(datalogger), writers_(writers), outbox_(outbox), clock_(clock)
{
}

void DataSenderEngine::apply_jobs(std::vector<ScheduledJob> jobs)
{
	const auto now = clock_.now();
	std::ranges::sort(jobs, {}, [](const auto &job) {
		return job.snapshot.job_id;
	});
	std::vector<ScheduledJobStatus> statuses;
	statuses.reserve(jobs.size());
	for (const auto &job : jobs) {
		if (job.snapshot.job_id.empty() || job.snapshot.revision == 0 ||
		    job.snapshot.generation_interval_nanoseconds <= 0)
			throw std::invalid_argument("scheduled job is invalid");
		ScheduledJobStatus status{.job_id = job.snapshot.job_id,
			.revision = job.snapshot.revision, .enabled = job.enabled,
			.next_window = std::nullopt,
			.last_generated_window = std::nullopt,
			.last_generated_at = 0,
			.last_error = {}};
		{
			std::scoped_lock lock(jobs_mutex_);
			const auto previous = std::ranges::find(statuses_, job.snapshot.job_id,
				&ScheduledJobStatus::job_id);
			if (previous != statuses_.end() &&
			    previous->revision == job.snapshot.revision)
				status = *previous;
			status.enabled = job.enabled;
		}
		if (job.enabled) {
			auto watermark = outbox_.watermark(job.snapshot.job_id);
			if (!watermark || watermark->job_revision != job.snapshot.revision) {
				watermark = JobWatermark{
					.job_id = job.snapshot.job_id,
					.job_revision = job.snapshot.revision,
					.completed_through = next_boundary(now,
						job.snapshot.generation_interval_nanoseconds),
				};
				outbox_.store_watermark(*watermark, now);
				status.last_error.clear();
				status.last_generated_window.reset();
				status.last_generated_at = 0;
			}
			status.next_window = UtcWindow{watermark->completed_through,
				watermark->completed_through +
					job.snapshot.generation_interval_nanoseconds};
		} else {
			status.next_window.reset();
		}
		statuses.push_back(std::move(status));
	}
	std::scoped_lock lock(jobs_mutex_);
	jobs_ = std::move(jobs);
	statuses_ = std::move(statuses);
}

GenerationRunResult DataSenderEngine::generate_due(
	std::size_t maximum_windows)
{
	if (maximum_windows == 0 || maximum_windows > 100)
		throw std::invalid_argument("generation batch limit is invalid");
	std::vector<ScheduledJob> jobs;
	{
		std::scoped_lock lock(jobs_mutex_);
		jobs = jobs_;
	}
	GenerationRunResult result;
	const auto now = clock_.now();
	for (const auto &job : jobs) {
		if (!job.enabled)
			continue;
		while (result.generated < maximum_windows) {
			auto watermark = outbox_.watermark(job.snapshot.job_id);
			if (!watermark || watermark->job_revision != job.snapshot.revision)
				break;
			const UtcWindow window{watermark->completed_through,
				watermark->completed_through +
					job.snapshot.generation_interval_nanoseconds};
			if (now < window.end + generation_settle_delay)
				break;
			try {
				auto dataset = datalogger_.generate(job.snapshot, window, now);
				auto writer = writers_.create(job.snapshot.format);
				auto generated = writer->write(dataset);
				outbox_.enqueue(dataset, generated, job.channel_ids,
					job.local_only);
				outbox_.store_watermark({job.snapshot.job_id,
					job.snapshot.revision, window.end}, now);
				++result.generated;
				std::scoped_lock lock(jobs_mutex_);
				const auto status = std::ranges::find(statuses_,
					job.snapshot.job_id, &ScheduledJobStatus::job_id);
				if (status != statuses_.end()) {
					status->last_generated_window = window;
					status->last_generated_at = now;
					status->last_error.clear();
					status->next_window = UtcWindow{window.end,
						window.end + job.snapshot.generation_interval_nanoseconds};
				}
			} catch (const DatalogError &error) {
				std::scoped_lock lock(jobs_mutex_);
				const auto status = std::ranges::find(statuses_,
					job.snapshot.job_id, &ScheduledJobStatus::job_id);
				if (status != statuses_.end())
					status->last_error = error.what();
				if (error.code() == DatalogErrorCode::StorageFailure)
					result.storage_blocked = true;
				else
					result.source_deferred = true;
				return result;
			} catch (const std::exception &) {
				std::scoped_lock lock(jobs_mutex_);
				const auto status = std::ranges::find(statuses_,
					job.snapshot.job_id, &ScheduledJobStatus::job_id);
				if (status != statuses_.end())
					status->last_error = "generation dependency is unavailable";
				result.source_deferred = true;
				return result;
			}
		}
	}
	return result;
}

std::size_t DataSenderEngine::deliver_due(const DeliveryExecutor &deliver,
	std::size_t maximum_deliveries)
{
	if (!deliver || maximum_deliveries == 0 || maximum_deliveries > 100)
		throw std::invalid_argument("delivery batch is invalid");
	const auto now = clock_.now();
	const auto deliveries = outbox_.claim_due(now, now + delivery_lease,
		maximum_deliveries);
	for (const auto &delivery : deliveries) {
		DeliveryResult result;
		try {
			auto generated = outbox_.content(delivery.artifact_id);
			result = deliver(delivery.channel_id,
				{delivery.channel_id, std::move(generated), false});
		} catch (const DatalogError &error) {
			result = {DeliveryDisposition::Blocked, {}, error.what()};
		} catch (const std::exception &) {
			result = {DeliveryDisposition::Retryable, {},
				"delivery dependency is unavailable"};
		}
		const auto attempt = delivery.attempt_count + 1;
		const auto next = result.disposition == DeliveryDisposition::Retryable
			? now + retry_delay(attempt,
				delivery.artifact_id + ":" + delivery.channel_id)
			: 0;
		outbox_.record_result(delivery, result, now, next);
	}
	return deliveries.size();
}

std::vector<ScheduledJobStatus> DataSenderEngine::job_status() const
{
	std::scoped_lock lock(jobs_mutex_);
	return statuses_;
}

UtcNanoseconds DataSenderEngine::retry_delay(std::uint32_t attempt_number,
	std::string_view stable_identity) noexcept
{
	if (attempt_number == 0)
		attempt_number = 1;
	const auto shift = std::min<std::uint32_t>(attempt_number - 1, 10);
	const auto base_seconds = std::min<std::uint64_t>(
		5ull << shift, 3600ull);
	const std::uint64_t remaining = 3600u - base_seconds;
	const std::uint64_t jitter_range = std::min<std::uint64_t>(
		base_seconds / 5u, remaining);
	const auto jitter = jitter_range == 0 ? 0 :
		fnv1a(stable_identity, attempt_number) % (jitter_range + 1);
	return static_cast<UtcNanoseconds>(base_seconds + jitter) * second;
}

UtcNanoseconds SystemClock::now() const noexcept
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace mnc::datalogger
