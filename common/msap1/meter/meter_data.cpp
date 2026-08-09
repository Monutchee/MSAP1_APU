#include "msap1/meter/meter_data.hpp"

#include <algorithm>
#include <condition_variable>
#include <limits>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

namespace msap1 {
namespace {

std::size_t period_index(MeasurementPeriod period)
{
	const auto index = static_cast<std::size_t>(period);
	if (index >= 4)
		throw std::invalid_argument("invalid measurement period");
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
	if (update.timing)
		slot->timing = update.timing;
	if (update.aggregate_timing)
		slot->aggregate_timing = update.aggregate_timing;
}

std::optional<MeterPeriodView>
MeterLatestStore::latest(MeasurementPeriod period) const
{
	std::scoped_lock lock(mutex_);
	return views_[period_index(period)];
}

struct MeterData::State {
	struct Subscriber {
		Subscriber(MeasurementPeriod requested_period,
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

		MeasurementPeriod period;

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

std::optional<MeterPeriodView> MeterData::latest(MeasurementPeriod period) const
{
	return state_->latest.latest(period);
}

MeterData::Subscription MeterData::subscribe(MeasurementPeriod period,
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

namespace {

/**
 * Channel and frequency decoding shared by the v1 and v2 record formats.
 * Word 6 is the only input that differs in meaning: configured window
 * samples (v1) versus actual block sample count (v2); either way it is the
 * sample count the PL accumulated into these values.
 */
FundamentalValues decode_fundamental_values(const MeterRecord &record,
					    std::uint64_t sequence,
					    SystemTime received_at,
					    SampleWindow window)
{
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
	return fundamental;
}

/**
 * Aggregate (MTR2) fundamental decoding. Channel order and micro-unit
 * encoding are identical to MTR1; only the word layout differs — two words
 * per channel at words 16..31, one mean-frequency word gated by status
 * bit 2, and channel validity from the word-7 mask (the AND across the 15
 * contributing blocks).
 */
FundamentalValues decode_aggregate_fundamental_values(const MeterRecord &record,
						      std::uint64_t sequence,
						      SystemTime received_at,
						      SampleWindow window)
{
	FundamentalValues fundamental{};
	const auto status = record.aggregate_status();
	/* Quality mapping is analogous to the basic decoder, reduced to the
	 * flags the aggregate record carries: a valid mean, an arithmetic
	 * overflow, or (when any of the 15 basic readings was missing)
	 * simply unavailable. */
	fundamental.frequency = {
		static_cast<std::int64_t>(record.aggregate_frequency_millihz()),
		status.frequency_valid
			? MeasurementQuality::valid
			: status.arithmetic_error
				? MeasurementQuality::arithmetic_error
				: MeasurementQuality::unavailable,
		sequence, received_at, window};

	const auto valid_mask = record.valid_mask();
	const auto current = [&](std::size_t channel) {
		return reading<MicroAmperes>(
			record.aggregate_rms_micro_units(channel),
			(valid_mask & (1u << channel)) != 0u, sequence,
			received_at, window);
	};
	const auto voltage = [&](std::size_t channel) {
		return reading<MicroVolts>(
			record.aggregate_rms_micro_units(channel),
			(valid_mask & (1u << channel)) != 0u, sequence,
			received_at, window);
	};
	/* Hardware channel order is Ia, Ib, Ic, In, Vc, Vb, Va, debug. */
	fundamental.current = {current(0), current(1), current(2), current(3)};
	fundamental.voltage_ln = {voltage(6), voltage(5), voltage(4)};
	return fundamental;
}

/** Actual per-block duration: sample_count / sample_rate (no fixed period). */
SampleWindow sample_window(std::uint32_t sample_count,
			   std::uint32_t sample_rate_hz)
{
	return {
		sample_count,
		sample_rate_hz == 0
			? std::chrono::nanoseconds{}
			: std::chrono::nanoseconds{
				  static_cast<std::int64_t>(sample_count) *
				  1'000'000'000ll /
				  static_cast<std::int64_t>(sample_rate_hz)},
	};
}

} // namespace

MeterUpdate decode_periodic_meter_record(const MeterRecord &record,
					 SystemTime received_at)
{
	if (!record.header_valid() ||
	    record.record_format() != meter_periodic_format)
		throw std::invalid_argument("invalid MTR1 v1 record");
	const auto sequence = static_cast<std::uint64_t>(record.sequence());
	const auto window =
		sample_window(record.window_samples(), record.sample_rate_hz());
	MeterUpdate update{};
	update.period = MeasurementPeriod::Basic;
	update.kind = RecordKind::fundamental;
	update.sequence = sequence;
	update.configuration_generation = record.configuration_generation();
	update.fundamental =
		decode_fundamental_values(record, sequence, received_at, window);
	/* v1 records predate cycle timing: no BlockTiming is available. */
	return update;
}

MeterUpdate decode_periodic_meter_record_v2(const MeterRecord &record,
					    SystemTime received_at)
{
	if (!record.header_valid() ||
	    record.record_format() != meter_periodic_format_v2)
		throw std::invalid_argument("invalid MTR1 v2 record");
	/*
	 * Validate the timing identity before building anything: malformed
	 * timing must never silently become a valid basic measurement block.
	 * The PL cannot emit these shapes today; this is defense in depth
	 * against a corrupted record or a future RTL regression.
	 */
	const auto timing_word = record.timing();
	if (timing_word.nominal_frequency_hz != 50u &&
	    timing_word.nominal_frequency_hz != 60u)
		throw std::invalid_argument(
			"invalid nominal frequency in MTR1 v2 timing word");
	const auto nominal = timing_word.nominal_frequency_hz == 50u
		? NominalFrequency::Hz50
		: NominalFrequency::Hz60;
	const auto sample_count = record.block_sample_count();
	if (sample_count == 0u)
		throw std::invalid_argument(
			"MTR1 v2 block has a zero sample count");
	const auto first_sample_index = record.first_sample_index();
	if (first_sample_index >
	    std::numeric_limits<std::uint64_t>::max() - sample_count)
		throw std::invalid_argument(
			"MTR1 v2 sample range overflows the 64-bit counter");
	/* A locked block is cycle-defined by construction: exactly the
	 * nominal's cycles-per-block. Fallback blocks are time-defined and
	 * may close any cycle count, including 0 or a partial tail. */
	if (timing_word.cycle_locked && !timing_word.free_run_fallback &&
	    timing_word.cycle_count != cycles_per_basic_block(nominal))
		throw std::invalid_argument(
			"MTR1 v2 cycle-locked block has an impossible cycle count");

	const auto sequence = static_cast<std::uint64_t>(record.sequence());
	/* v2 word 6 is the ACTUAL sample count of this cycle-defined block. */
	const auto window = sample_window(sample_count, record.sample_rate_hz());
	MeterUpdate update{};
	update.period = MeasurementPeriod::Basic;
	update.kind = RecordKind::fundamental;
	update.sequence = sequence;
	update.configuration_generation = record.configuration_generation();
	update.fundamental =
		decode_fundamental_values(record, sequence, received_at, window);

	BlockTiming timing{};
	timing.sequence = sequence;
	timing.configuration_generation = record.configuration_generation();
	timing.first_sample_index = first_sample_index;
	timing.sample_count = sample_count;
	timing.cycle_count = timing_word.cycle_count;
	timing.nominal_frequency = nominal;
	timing.cycle_locked = timing_word.cycle_locked;
	timing.free_run_fallback = timing_word.free_run_fallback;
	timing.first_block_after_apply = timing_word.first_block_after_apply;
	/* TimeQuality/utc_start/utc_uncertainty_ns are stamped by the caller:
	 * UTC state lives in the APU MeasurementTimebase, never in the PL
	 * record. */
	update.timing = timing;
	return update;
}

MeterUpdate decode_aggregate_meter_record(const MeterRecord &record,
					  SystemTime received_at)
{
	if (!record.header_valid() ||
	    record.record_format() != meter_aggregate_format)
		throw std::invalid_argument("invalid MTR2 aggregate record");
	/*
	 * Validate the aggregation identity before building anything,
	 * mirroring the hardened v2 rules: the PL emits only complete
	 * 15-block aggregates whose cycle count follows the nominal, so any
	 * other shape is corruption or a future RTL regression and must
	 * never silently decode into a valid aggregate.
	 */
	const auto composition = record.aggregate_composition();
	if (composition.nominal_frequency_hz != 50u &&
	    composition.nominal_frequency_hz != 60u)
		throw std::invalid_argument(
			"invalid nominal frequency in MTR2 composition word");
	const auto nominal = composition.nominal_frequency_hz == 50u
		? NominalFrequency::Hz50
		: NominalFrequency::Hz60;
	if (composition.basic_block_count != meter::basic_blocks_per_aggregate)
		throw std::invalid_argument(
			"MTR2 aggregate is not built from exactly 15 basic blocks");
	if (composition.cycle_count != cycles_per_aggregate(nominal))
		throw std::invalid_argument(
			"MTR2 aggregate cycle count does not match its nominal");
	const auto sample_count = record.aggregate_sample_count();
	if (sample_count == 0u)
		throw std::invalid_argument(
			"MTR2 aggregate has a zero sample count");
	const auto first_sample_index = record.aggregate_first_sample_index();
	if (first_sample_index >
	    std::numeric_limits<std::uint64_t>::max() - sample_count)
		throw std::invalid_argument(
			"MTR2 sample range overflows the 64-bit counter");

	const auto sequence =
		static_cast<std::uint64_t>(record.aggregate_sequence());
	/* The aggregate has no fixed duration either: ~3 s nominally, but
	 * defined as 150/180 cycles, so the window follows the actual total
	 * sample count. */
	const auto window = sample_window(sample_count, record.sample_rate_hz());
	MeterUpdate update{};
	update.period = MeasurementPeriod::Cycles150_180;
	update.kind = RecordKind::fundamental;
	update.sequence = sequence;
	update.configuration_generation = record.configuration_generation();
	update.fundamental = decode_aggregate_fundamental_values(
		record, sequence, received_at, window);

	const auto status = record.aggregate_status();
	meter::AggregateTiming timing{};
	timing.sequence = sequence;
	timing.configuration_generation = record.configuration_generation();
	timing.first_sample_index = first_sample_index;
	timing.sample_count = sample_count;
	timing.first_basic_sequence = record.first_basic_sequence();
	timing.last_basic_sequence = record.last_basic_sequence();
	timing.basic_block_count = composition.basic_block_count;
	timing.cycle_count = composition.cycle_count;
	timing.nominal_frequency = nominal;
	timing.arithmetic_error = status.arithmetic_error;
	timing.frequency_valid = status.frequency_valid;
	/* TimeQuality/utc_start/utc_uncertainty_ns are stamped by the caller:
	 * UTC state lives in the APU MeasurementTimebase, never in the PL
	 * record. */
	update.aggregate_timing = timing;
	return update;
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
	/* v1 stays registered: stored streams may replay 0x00010001 records. */
	result.register_decoder(meter_periodic_format,
		[](const MeterRecord &record, SystemTime received_at) {
			return decode_periodic_meter_record(record, received_at);
		});
	result.register_decoder(meter_periodic_format_v2,
		[](const MeterRecord &record, SystemTime received_at) {
			return decode_periodic_meter_record_v2(record,
							       received_at);
		});
	/* MTR2 aggregates interleave with basic records on the same DMA
	 * stream; the registry routes them by the format word. */
	result.register_decoder(meter_aggregate_format,
		[](const MeterRecord &record, SystemTime received_at) {
			return decode_aggregate_meter_record(record,
							     received_at);
		});
	return result;
}

} // namespace msap1
