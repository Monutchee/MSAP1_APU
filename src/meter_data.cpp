#include "msap1/meter_data.hpp"

#include <algorithm>
#include <condition_variable>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

namespace msap1 {
namespace {

std::size_t period_index(UpdatePeriod period)
{
	const auto index = static_cast<std::size_t>(period);
	if (index >= 6)
		throw std::invalid_argument("invalid meter update period");
	return index;
}

template<typename Unit>
Reading<Unit> reading(std::int64_t value, bool valid, std::uint64_t sequence,
		      SystemTime timestamp, SampleWindow window)
{
	return {value,
		valid ? MeasurementQuality::valid : MeasurementQuality::unavailable,
		sequence, timestamp, window};
}

} // namespace

std::chrono::milliseconds duration(UpdatePeriod period)
{
	using namespace std::chrono_literals;
	switch (period) {
	case UpdatePeriod::ms200:
		return 200ms;
	case UpdatePeriod::s1:
		return 1s;
	case UpdatePeriod::s3:
		return 3s;
	case UpdatePeriod::s10:
		return 10s;
	case UpdatePeriod::min10:
		return 10min;
	case UpdatePeriod::h2:
		return 2h;
	}
	throw std::invalid_argument("invalid meter update period");
}

std::optional<UpdatePeriod> update_period(std::chrono::milliseconds value)
{
	for (const auto period : {UpdatePeriod::ms200, UpdatePeriod::s1,
				 UpdatePeriod::s3, UpdatePeriod::s10,
				 UpdatePeriod::min10, UpdatePeriod::h2}) {
		if (duration(period) == value)
			return period;
	}
	return std::nullopt;
}

void MeterLatestStore::apply(const MeterUpdate &update)
{
	std::scoped_lock lock(mutex_);
	auto &slot = views_[period_index(update.period)];
	if (!slot)
		slot = MeterPeriodView{.period = update.period};
	if (update.sequence < slot->latest_sequence)
		return;
	slot->latest_sequence = update.sequence;
	slot->configuration_generation = update.configuration_generation;
	slot->updated_at = std::chrono::system_clock::now();
	if (update.fundamental)
		slot->values.fundamental = *update.fundamental;
	if (update.power)
		slot->values.power = *update.power;
	if (update.energy)
		slot->values.energy = *update.energy;
	if (update.demand)
		slot->values.demand = *update.demand;
	if (update.power_quality)
		slot->values.power_quality = *update.power_quality;
}

std::optional<MeterPeriodView>
MeterLatestStore::latest(UpdatePeriod period) const
{
	std::scoped_lock lock(mutex_);
	return views_[period_index(period)];
}

struct MeterData::State {
	struct Subscriber {
		Subscriber(UpdatePeriod requested_period,
			   UpdateCallback requested_callback)
			: period(requested_period),
			  callback(std::move(requested_callback)),
			  worker([this](std::stop_token stop_token) {
				  run(stop_token);
			  })
		{
		}

		Subscriber(const Subscriber &) = delete;
		Subscriber &operator=(const Subscriber &) = delete;

		~Subscriber()
		{
			stop();
		}

		void publish(const MeterPeriodView &view)
		{
			{
				std::scoped_lock lock(mutex);
				if (!accepting)
					return;
				/* A latest-state consumer is deliberately lossy. Replacing
				 * an unread view prevents a slow publisher from applying
				 * backpressure to durable record ingestion. */
				pending = view;
			}
			condition.notify_one();
		}

		void stop() noexcept
		{
			{
				std::scoped_lock lock(mutex);
				if (!accepting)
					return;
				accepting = false;
				pending.reset();
			}
			worker.request_stop();
			condition.notify_all();
		}

		UpdatePeriod period;

	private:
		void run(std::stop_token stop_token) noexcept
		{
			while (!stop_token.stop_requested()) {
				std::optional<MeterPeriodView> view;
				{
					std::unique_lock lock(mutex);
					if (!condition.wait(lock, stop_token, [this] {
						    return pending.has_value() || !accepting;
					    }))
						return;
					if (!accepting)
						return;
					view = std::move(pending);
					pending.reset();
				}
				try {
					callback(*view);
				} catch (...) {
					/* Subscribers are observational. A consumer exception
					 * must not terminate acquisition or its worker thread. */
				}
			}
		}

		UpdateCallback callback;
		std::mutex mutex;
		std::condition_variable_any condition;
		std::optional<MeterPeriodView> pending;
		bool accepting = true;
		std::jthread worker;
	};

	MeterLatestStore latest;
	std::mutex mutex;
	std::uint64_t next_id = 1;
	std::unordered_map<std::uint64_t, std::shared_ptr<Subscriber>> subscribers;
};

MeterData::Subscription::Subscription(std::weak_ptr<void> state,
				      std::uint64_t id)
	: state_(std::move(state)), id_(id)
{
}

MeterData::Subscription::Subscription(Subscription &&other) noexcept
	: state_(std::move(other.state_)), id_(std::exchange(other.id_, 0))
{
}

MeterData::Subscription &
MeterData::Subscription::operator=(Subscription &&other) noexcept
{
	if (this == &other)
		return *this;
	Subscription replacement(std::move(other));
	state_.swap(replacement.state_);
	std::swap(id_, replacement.id_);
	return *this;
}

MeterData::Subscription::~Subscription()
{
	auto opaque = state_.lock();
	if (!opaque || id_ == 0)
		return;
	auto state = std::static_pointer_cast<MeterData::State>(opaque);
	std::shared_ptr<MeterData::State::Subscriber> subscriber;
	{
		std::scoped_lock lock(state->mutex);
		const auto found = state->subscribers.find(id_);
		if (found == state->subscribers.end())
			return;
		subscriber = std::move(found->second);
		state->subscribers.erase(found);
	}
	subscriber->stop();
}

MeterData::MeterData() : state_(std::make_shared<State>()) {}

void MeterData::apply(const MeterUpdate &update)
{
	state_->latest.apply(update);
	const auto view = state_->latest.latest(update.period);
	if (!view)
		return;
	std::vector<std::shared_ptr<State::Subscriber>> subscribers;
	{
		std::scoped_lock lock(state_->mutex);
		for (const auto &[id, subscriber] : state_->subscribers) {
			(void)id;
			if (subscriber->period == update.period)
				subscribers.push_back(subscriber);
		}
	}
	for (const auto &subscriber : subscribers)
		subscriber->publish(*view);
}

std::optional<MeterPeriodView> MeterData::latest(UpdatePeriod period) const
{
	return state_->latest.latest(period);
}

MeterData::Subscription MeterData::subscribe(UpdatePeriod period,
					      UpdateCallback callback)
{
	if (!callback)
		throw std::invalid_argument("meter subscription callback is empty");
	std::scoped_lock lock(state_->mutex);
	const auto id = state_->next_id++;
	state_->subscribers.emplace(
		id, std::make_shared<State::Subscriber>(period, std::move(callback)));
	return Subscription{state_, id};
}

MeterUpdate decode_periodic_meter_record(const MeterRecord &record,
					 SystemTime received_at)
{
	if (!record.header_valid())
		throw std::invalid_argument("invalid MTR1 record");
	const auto sequence = static_cast<std::uint64_t>(record.sequence());
	const SampleWindow window{
		record.window_samples(),
		record.sample_rate_hz() == 0
			? std::chrono::nanoseconds{}
			: std::chrono::nanoseconds{
				  static_cast<std::int64_t>(record.window_samples()) *
				  1'000'000'000ll /
				  static_cast<std::int64_t>(record.sample_rate_hz())},
	};
	FundamentalValues fundamental{};
	const auto frequency = record.frequency();
	fundamental.frequency = {
		static_cast<std::int64_t>(frequency.millihz),
		frequency.valid
			? MeasurementQuality::valid
			: frequency.arithmetic_error
				? MeasurementQuality::arithmetic_error
				: frequency.out_of_range
					? MeasurementQuality::out_of_range
					: frequency.timed_out
						? MeasurementQuality::timed_out
						: MeasurementQuality::unavailable,
		sequence, received_at, window};

	/* Hardware channel order is Ia, Ib, Ic, In, Vc, Vb, Va, debug. */
	const auto ia = record.channel(0);
	const auto ib = record.channel(1);
	const auto ic = record.channel(2);
	const auto in = record.channel(3);
	const auto vc = record.channel(4);
	const auto vb = record.channel(5);
	const auto va = record.channel(6);
	fundamental.current = {
		reading<MicroAmperes>(ia.rms_micro_units, ia.valid, sequence,
					 received_at, window),
		reading<MicroAmperes>(ib.rms_micro_units, ib.valid, sequence,
					 received_at, window),
		reading<MicroAmperes>(ic.rms_micro_units, ic.valid, sequence,
					 received_at, window),
		reading<MicroAmperes>(in.rms_micro_units, in.valid, sequence,
					 received_at, window),
	};
	fundamental.voltage_ln = {
		reading<MicroVolts>(va.rms_micro_units, va.valid, sequence,
				      received_at, window),
		reading<MicroVolts>(vb.rms_micro_units, vb.valid, sequence,
				      received_at, window),
		reading<MicroVolts>(vc.rms_micro_units, vc.valid, sequence,
				      received_at, window),
	};
	return {
		UpdatePeriod::ms200,
		RecordKind::fundamental,
		sequence,
		record.configuration_generation(),
		std::move(fundamental),
		std::nullopt,
		std::nullopt,
		std::nullopt,
		std::nullopt,
	};
}

void MeterDecoderRegistry::register_decoder(std::uint32_t record_format,
					      Decoder decoder)
{
	if (!decoder)
		throw std::invalid_argument("meter record decoder is empty");
	if (!decoders_.emplace(record_format, std::move(decoder)).second)
		throw std::invalid_argument("duplicate meter record decoder");
}

MeterUpdate MeterDecoderRegistry::decode(const MeterRecord &record,
					 SystemTime received_at) const
{
	const auto found = decoders_.find(record.word(1));
	if (found == decoders_.end())
		throw std::invalid_argument("unsupported meter record format");
	return found->second(record, received_at);
}

MeterDecoderRegistry MeterDecoderRegistry::with_builtin_decoders()
{
	MeterDecoderRegistry result;
	result.register_decoder(meter_periodic_format,
		[](const MeterRecord &record, SystemTime received_at) {
			return decode_periodic_meter_record(record, received_at);
		});
	return result;
}

} // namespace msap1
