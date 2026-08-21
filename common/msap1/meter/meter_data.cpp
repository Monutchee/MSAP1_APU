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

struct AggregatePayloadState {
	bool arithmetic_error = false;
	bool invalid = false;
};

AggregatePayloadState aggregate_payload_state(const MeterRecord &record,
					       MeasurementPeriod period)
{
	if (period != MeasurementPeriod::Min10 &&
	    period != MeasurementPeriod::Hour2)
		return {};
	const auto status = period == MeasurementPeriod::Hour2
		? record.two_hour_status()
		: record.ten_minute_status();
	/* Bit 1 is the complete flag only on the fundamental record. The
	 * phasor and unbalance siblings retain their established bit-1
	 * phasor-invalid meaning, so their interval validity is carried by
	 * the shared aligned/contaminated/boundary bits instead. */
	const bool incomplete =
		(record.record_format() == meter_ten_minute_format ||
		 record.record_format() == meter_two_hour_format) &&
		!status.complete;
	return {
		status.arithmetic_error,
		incomplete || !status.time_aligned || status.contaminated ||
			!status.boundary_valid,
	};
}

template<typename Unit>
Reading<Unit> payload_reading(std::int64_t value, bool available,
			      AggregatePayloadState state,
			      std::uint64_t sequence, SystemTime timestamp,
			      SampleWindow window)
{
	const auto quality = state.arithmetic_error
		? MeasurementQuality::arithmetic_error
		: state.invalid ? MeasurementQuality::invalid
				: available ? MeasurementQuality::valid
					    : MeasurementQuality::unavailable;
	return {value, quality, sequence, timestamp, window};
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
	if (update.phasor)
		slot->values.phasor = *update.phasor;
	if (update.unbalance)
		slot->values.unbalance = *update.unbalance;
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
 * Channel and frequency decoding for the periodic (MTR1) record format.
 * Word 6 is the actual block sample count — the sample count the PL
 * accumulated into these values.
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
	/* BASIC-v4 words 51..53: Vab, Vbc, Vca micro-units (32-bit). A pair
	 * is valid only when both of its lanes are. */
	fundamental.voltage_ll = {
		reading<MicroVolts>(record.word(51), va.valid && vb.valid,
				      sequence, received_at, window),
		reading<MicroVolts>(record.word(52), vb.valid && vc.valid,
				      sequence, received_at, window),
		reading<MicroVolts>(record.word(53), vc.valid && va.valid,
				      sequence, received_at, window),
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
/*
 * An aggregate RMS reading is only trustworthy when the channel took part
 * in every contributing basic block AND the aggregation arithmetic itself
 * did not saturate. An arithmetic error therefore outranks the channel
 * mask: publishing a saturated aggregate as MeasurementQuality::valid
 * would hide the fault from every consumer.
 */
template<typename Unit>
Reading<Unit> aggregate_reading(std::int64_t value, bool channel_valid,
				bool arithmetic_error, bool force_invalid,
				std::uint64_t sequence,
				SystemTime timestamp, SampleWindow window)
{
	const auto quality = arithmetic_error
		? MeasurementQuality::arithmetic_error
		: force_invalid ? MeasurementQuality::invalid
		: channel_valid ? MeasurementQuality::valid
				: MeasurementQuality::unavailable;
	return {value, quality, sequence, timestamp, window};
}

FundamentalValues decode_aggregate_fundamental_values(const MeterRecord &record,
						      std::uint64_t sequence,
						      SystemTime received_at,
						      SampleWindow window,
						      bool arithmetic_error,
						      bool force_invalid)
{
	FundamentalValues fundamental{};
	/*
	 * The MTR2 frequency field is informational only: it is the mean of
	 * the 15 basic frequency estimates, not a standardized measurement.
	 * IEC 61000-4-30 defines the frequency product over its own (10 s)
	 * interval, which will be implemented with that interval in a later
	 * milestone. The informative value is carried through for
	 * diagnostics, but it must never be advertised as a valid Class A
	 * frequency result, so its quality stays unavailable regardless of
	 * the record's frequency-valid flag. That flag remains visible to
	 * diagnostics through AggregateTiming::frequency_valid.
	 */
	fundamental.frequency = {
		static_cast<std::int64_t>(record.aggregate_frequency_millihz()),
		MeasurementQuality::unavailable, sequence, received_at, window};

	const auto valid_mask = record.valid_mask();
	const auto current = [&](std::size_t channel) {
		return aggregate_reading<MicroAmperes>(
			record.aggregate_rms_micro_units(channel),
			(valid_mask & (1u << channel)) != 0u,
			arithmetic_error, force_invalid, sequence, received_at,
			window);
	};
	const auto voltage = [&](std::size_t channel) {
		return aggregate_reading<MicroVolts>(
			record.aggregate_rms_micro_units(channel),
			(valid_mask & (1u << channel)) != 0u,
			arithmetic_error, force_invalid, sequence, received_at,
			window);
	};
	/* Hardware channel order is Ia, Ib, Ic, In, Vc, Vb, Va, debug. */
	fundamental.current = {current(0), current(1), current(2), current(3)};
	fundamental.voltage_ln = {voltage(6), voltage(5), voltage(4)};
	/* AGG-v3 (M11): line-line RMS at words 38..40, pair-valid when both
	 * contributing lanes are (with the arithmetic gate above). */
	const auto voltage_pair = [&](std::size_t word, unsigned lane_a,
				      unsigned lane_b) {
		return aggregate_reading<MicroVolts>(
			static_cast<std::int64_t>(record.word(word)),
			(valid_mask & (1u << lane_a)) != 0u &&
				(valid_mask & (1u << lane_b)) != 0u,
			arithmetic_error, force_invalid, sequence, received_at,
			window);
	};
	fundamental.voltage_ll = {voltage_pair(38, 6, 5), voltage_pair(39, 5, 4),
				  voltage_pair(40, 4, 6)};
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

namespace {

/* The POWER payload map (words 16+) is shared verbatim by the basic
 * (0x0007) and aggregate (0x0010) periods — one decoder, two wrappers. */
MeterUpdate decode_power_payload(const MeterRecord &record,
				 SystemTime received_at,
				 std::uint32_t expected_format,
				 MeasurementPeriod period)
{
	if (!record.header_valid() ||
	    record.record_format() != expected_format)
		throw std::invalid_argument("invalid power record");
	const auto sample_count = record.block_sample_count();
	if (sample_count == 0u)
		throw std::invalid_argument("power record has a zero sample count");
	if (record.first_sample_index() == 0u)
		throw std::invalid_argument(
			"power record has a zero first-sample index");

	const auto sequence = static_cast<std::uint64_t>(record.sequence());
	const auto window = sample_window(sample_count, record.sample_rate_hz());
	const auto payload_state = aggregate_payload_state(record, period);
	const auto valid_mask = record.valid_mask();
	const auto lane_valid = [&](unsigned lane) {
		return (valid_mask & (1u << lane)) != 0u;
	};
	const auto word_s64 = [&](std::size_t low) {
		return static_cast<std::int64_t>(
			record.word(low) |
			(std::uint64_t{record.word(low + 1)} << 32));
	};

	/* Hardware lane order: Ia..In = 0..3, Vc/Vb/Va = 4/5/6; phases pair
	 * Va/Ia, Vb/Ib, Vc/Ic (the PL power core's mapping). */
	static constexpr unsigned voltage_lane[3] = {6, 5, 4};
	static constexpr unsigned current_lane[3] = {0, 1, 2};
	PowerValues values{};
	Reading<Picowatts> phase_p[3];
	Reading<PicoVoltAmperes> phase_s[3];
	Reading<PowerFactorMillionths> phase_pf[3];
	for (std::size_t phase = 0; phase < 3; ++phase) {
		const std::size_t base = 16u + phase * 5u;
		const bool valid = lane_valid(voltage_lane[phase]) &&
				   lane_valid(current_lane[phase]);
		const auto p = word_s64(base);
		const auto s_va = word_s64(base + 2);
		const auto pf = static_cast<std::int32_t>(record.word(base + 4));
		phase_p[phase] = payload_reading<Picowatts>(
			p, valid, payload_state, sequence, received_at, window);
		phase_s[phase] = payload_reading<PicoVoltAmperes>(
			s_va, valid, payload_state, sequence, received_at, window);
		/* PF is undefined at S == 0: publish it as unavailable, not
		 * as a confident zero. */
		phase_pf[phase] = payload_reading<PowerFactorMillionths>(
			pf, valid && s_va != 0, payload_state, sequence,
			received_at, window);
	}
	values.active_power = {phase_p[0], phase_p[1], phase_p[2]};
	values.apparent_power = {phase_s[0], phase_s[1], phase_s[2]};
	values.power_factor = {phase_pf[0], phase_pf[1], phase_pf[2]};
	const bool totals_valid = lane_valid(6) && lane_valid(0);
	const auto total_s = word_s64(33);
	values.total_active_power = payload_reading<Picowatts>(
		word_s64(31), totals_valid, payload_state, sequence,
		received_at, window);
	values.total_apparent_power = payload_reading<PicoVoltAmperes>(
		total_s, totals_valid, payload_state, sequence, received_at,
		window);
	values.total_power_factor = payload_reading<PowerFactorMillionths>(
		static_cast<std::int32_t>(record.word(35)),
		totals_valid && total_s != 0, payload_state, sequence,
		received_at, window);
	const auto crest = [&](unsigned lane) {
		return payload_reading<CrestTenThousandths>(
			record.word(36u + lane), lane_valid(lane), payload_state,
			sequence, received_at, window);
	};
	values.current_crest = {crest(0), crest(1), crest(2), crest(3)};
	values.voltage_crest = {crest(6), crest(5), crest(4)};

	MeterUpdate update{};
	update.period = period;
	update.kind = RecordKind::power;
	update.sequence = sequence;
	update.configuration_generation = record.configuration_generation();
	update.power = values;
	return update;
}

} // namespace

MeterUpdate decode_power_meter_record(const MeterRecord &record,
				      SystemTime received_at)
{
	return decode_power_payload(record, received_at, meter_power_format,
				    MeasurementPeriod::Basic);
}

MeterUpdate decode_aggregate_power_meter_record(const MeterRecord &record,
						SystemTime received_at)
{
	return decode_power_payload(record, received_at,
				    meter_aggregate_power_format,
				    MeasurementPeriod::Cycles150_180);
}

MeterUpdate decode_ten_minute_power_meter_record(const MeterRecord &record,
						 SystemTime received_at)
{
	return decode_power_payload(record, received_at,
				    meter_ten_minute_power_format,
				    MeasurementPeriod::Min10);
}

MeterUpdate decode_two_hour_power_meter_record(const MeterRecord &record,
					       SystemTime received_at)
{
	return decode_power_payload(record, received_at,
				    meter_two_hour_power_format,
				    MeasurementPeriod::Hour2);
}

namespace {

MeterUpdate decode_phasor_payload(const MeterRecord &record,
				  SystemTime received_at,
				  std::uint32_t expected_format,
				  MeasurementPeriod period)
{
	/* Word map: PL contract in MSAP1_PL .../common/include/
	 * measurement_record.hpp (PHASOR-v1 / AGG-PHASOR-v1). */
	if (!record.header_valid() ||
	    record.record_format() != expected_format)
		throw std::invalid_argument("invalid phasor record");
	const auto sample_count = record.block_sample_count();
	if (sample_count == 0u)
		throw std::invalid_argument(
			"phasor record has a zero sample count");
	if (record.first_sample_index() == 0u)
		throw std::invalid_argument(
			"phasor record has a zero first-sample index");

	const auto sequence = static_cast<std::uint64_t>(record.sequence());
	const auto window = sample_window(sample_count, record.sample_rate_hz());
	const auto payload_state = aggregate_payload_state(record, period);
	const auto valid_mask = record.valid_mask();
	/* Status bit 1: a merged cycle lacked a frequency reference — the
	 * values exist but are garbage, which is `invalid`, never a silent
	 * `unavailable`. */
	const bool block_invalid = (record.word(8) & 0x2u) != 0u ||
				   payload_state.invalid;
	const auto flags = record.word(51);
	const bool reference_valid = (flags & 0x100u) != 0u;
	const auto lane_valid = [&](unsigned lane) {
		return (valid_mask & (1u << lane)) != 0u;
	};
	const auto quality = [&](bool available) {
		if (payload_state.arithmetic_error)
			return MeasurementQuality::arithmetic_error;
		if (block_invalid)
			return MeasurementQuality::invalid;
		return available ? MeasurementQuality::valid
				 : MeasurementQuality::unavailable;
	};
	const auto fund_of = [&](unsigned lane) {
		return static_cast<std::int64_t>(record.word(16u + lane * 2u));
	};
	/* Angles are u32 millidegrees in [0, 360000) since PHASOR-v2. */
	const auto angle_of = [&](unsigned lane) {
		return static_cast<std::int64_t>(record.word(17u + lane * 2u));
	};
	/* An angle is meaningful only against a live Va reference and for a
	 * nonzero phasor; differences (displacement) need neither reference. */
	const auto angle_available = [&](unsigned lane) {
		return lane_valid(lane) && reference_valid && fund_of(lane) != 0;
	};

	PhasorValues values{};
	values.phasor_invalid = block_invalid;
	values.angle_reference_valid = reference_valid;

	/* Hardware lane order: Ia..In = 0..3, Vc/Vb/Va = 4/5/6. */
	const auto fund_v = [&](unsigned lane) {
		return Reading<MicroVolts>{fund_of(lane),
					   quality(lane_valid(lane)), sequence,
					   received_at, window};
	};
	const auto fund_i = [&](unsigned lane) {
		return Reading<MicroAmperes>{fund_of(lane),
					     quality(lane_valid(lane)), sequence,
					     received_at, window};
	};
	const auto angle = [&](unsigned lane) {
		return Reading<Millidegrees>{angle_of(lane),
					     quality(angle_available(lane)),
					     sequence, received_at, window};
	};
	values.fundamental_voltage = {fund_v(6), fund_v(5), fund_v(4)};
	values.fundamental_current = {fund_i(0), fund_i(1), fund_i(2), fund_i(3)};
	values.voltage_angle = {angle(6), angle(5), angle(4)};
	values.current_angle = {angle(0), angle(1), angle(2), angle(3)};

	/* Line-line phasors, pairs AB/BC/CA at words 30..35. */
	static constexpr unsigned pair_lane_a[3] = {6, 5, 4};
	static constexpr unsigned pair_lane_b[3] = {5, 4, 6};
	Reading<MicroVolts> vll_fund[3];
	Reading<Millidegrees> vll_angle[3];
	for (std::size_t pair = 0; pair < 3; ++pair) {
		const bool pair_valid = lane_valid(pair_lane_a[pair]) &&
					lane_valid(pair_lane_b[pair]);
		const auto fund = static_cast<std::int64_t>(
			record.word(30u + pair * 2u));
		vll_fund[pair] = {fund, quality(pair_valid), sequence,
				  received_at, window};
		vll_angle[pair] = {
			static_cast<std::int64_t>(record.word(31u + pair * 2u)),
			quality(pair_valid && reference_valid && fund != 0),
			sequence, received_at, window};
	}
	values.fundamental_voltage_ll = {vll_fund[0], vll_fund[1], vll_fund[2]};
	values.voltage_ll_angle = {vll_angle[0], vll_angle[1], vll_angle[2]};

	/* Per-phase quantities: Va/Ia, Vb/Ib, Vc/Ic. The load-nature code is
	 * the exact S1 = 0 gate (undefined <=> S1 = 0), so it gates the
	 * displacement PF the same way the PL defined it. */
	static constexpr unsigned voltage_lane[3] = {6, 5, 4};
	static constexpr unsigned current_lane[3] = {0, 1, 2};
	Reading<Millidegrees> disp[3];
	Reading<Picovars> q1[3];
	Reading<Picowatts> p1[3];
	Reading<PowerFactorMillionths> dpf[3];
	LoadNature nature[3];
	for (std::size_t phase = 0; phase < 3; ++phase) {
		const unsigned v = voltage_lane[phase];
		const unsigned i = current_lane[phase];
		const bool pair_valid = lane_valid(v) && lane_valid(i);
		nature[phase] = static_cast<LoadNature>(
			(flags >> (phase * 2u)) & 0x3u);
		const bool defined =
			pair_valid && nature[phase] != LoadNature::undefined;
		disp[phase] = {
			static_cast<std::int64_t>(record.word(36u + phase)),
			quality(pair_valid && fund_of(v) != 0 &&
				fund_of(i) != 0),
			sequence, received_at, window};
		q1[phase] = {record.signed64(39u + phase * 2u),
			     quality(pair_valid), sequence, received_at,
			     window};
		p1[phase] = {record.signed64(52u + phase * 2u),
			     quality(pair_valid), sequence, received_at,
			     window};
		dpf[phase] = {
			static_cast<std::int64_t>(static_cast<std::int32_t>(
				record.word(47u + phase))),
			quality(defined), sequence, received_at, window};
	}
	values.displacement_angle = {disp[0], disp[1], disp[2]};
	values.reactive_power = {q1[0], q1[1], q1[2]};
	values.fundamental_active_power = {p1[0], p1[1], p1[2]};
	values.displacement_power_factor = {dpf[0], dpf[1], dpf[2]};
	values.load_nature = {nature[0], nature[1], nature[2]};
	values.total_load_nature = static_cast<LoadNature>((flags >> 6u) & 0x3u);

	const bool totals_valid = lane_valid(6) && lane_valid(0);
	values.total_reactive_power = {record.signed64(45u),
				       quality(totals_valid), sequence,
				       received_at, window};
	values.total_fundamental_active_power = {record.signed64(58u),
						 quality(totals_valid),
						 sequence, received_at, window};
	values.total_displacement_power_factor = {
		static_cast<std::int64_t>(
			static_cast<std::int32_t>(record.word(50u))),
		quality(totals_valid &&
			values.total_load_nature != LoadNature::undefined),
		sequence, received_at, window};

	MeterUpdate update{};
	update.period = period;
	update.kind = RecordKind::phasor;
	update.sequence = sequence;
	update.configuration_generation = record.configuration_generation();
	update.phasor = values;
	return update;
}

} // namespace

MeterUpdate decode_phasor_meter_record(const MeterRecord &record,
				       SystemTime received_at)
{
	return decode_phasor_payload(record, received_at, meter_phasor_format,
				     MeasurementPeriod::Basic);
}

MeterUpdate decode_aggregate_phasor_meter_record(const MeterRecord &record,
						 SystemTime received_at)
{
	return decode_phasor_payload(record, received_at,
				     meter_aggregate_phasor_format,
				     MeasurementPeriod::Cycles150_180);
}

MeterUpdate decode_ten_minute_phasor_meter_record(const MeterRecord &record,
						  SystemTime received_at)
{
	return decode_phasor_payload(record, received_at,
				     meter_ten_minute_phasor_format,
				     MeasurementPeriod::Min10);
}

MeterUpdate decode_two_hour_phasor_meter_record(const MeterRecord &record,
					        SystemTime received_at)
{
	return decode_phasor_payload(record, received_at,
				     meter_two_hour_phasor_format,
				     MeasurementPeriod::Hour2);
}

namespace {

MeterUpdate decode_unbalance_payload(const MeterRecord &record,
				     SystemTime received_at,
				     std::uint32_t expected_format,
				     MeasurementPeriod period)
{
	/* Word map: PL contract in MSAP1_PL .../common/include/
	 * measurement_record.hpp (UNBALANCE-v1 / AGG-UNBAL-v1). */
	if (!record.header_valid() ||
	    record.record_format() != expected_format)
		throw std::invalid_argument("invalid unbalance record");
	const auto sample_count = record.block_sample_count();
	if (sample_count == 0u)
		throw std::invalid_argument(
			"unbalance record has a zero sample count");
	if (record.first_sample_index() == 0u)
		throw std::invalid_argument(
			"unbalance record has a zero first-sample index");

	const auto sequence = static_cast<std::uint64_t>(record.sequence());
	const auto window = sample_window(sample_count, record.sample_rate_hz());
	const auto payload_state = aggregate_payload_state(record, period);
	const bool block_invalid = (record.word(8) & 0x2u) != 0u ||
				   payload_state.invalid;
	const auto flags = record.word(32);
	const bool v_ratios_valid = (flags & 0x1u) != 0u;
	const bool i_ratios_valid = (flags & 0x2u) != 0u;
	const bool reference_valid = (flags & 0x100u) != 0u;
	const auto valid_mask = record.valid_mask();
	/* A sequence set is meaningful only when all three of its phase
	 * lanes contributed (the components mix every phase). */
	const bool v_lanes = (valid_mask & 0x70u) == 0x70u; /* Vc/Vb/Va */
	const bool i_lanes = (valid_mask & 0x07u) == 0x07u; /* Ia/Ib/Ic */
	const auto quality = [&](bool available) {
		if (payload_state.arithmetic_error)
			return MeasurementQuality::arithmetic_error;
		if (block_invalid)
			return MeasurementQuality::invalid;
		return available ? MeasurementQuality::valid
				 : MeasurementQuality::unavailable;
	};

	UnbalanceValues values{};
	values.phasor_invalid = block_invalid;
	values.angle_reference_valid = reference_valid;

	const auto magnitude = [&](std::size_t base, bool lanes_ok) {
		return std::pair<std::int64_t, MeasurementQuality>{
			static_cast<std::int64_t>(record.word(base)),
			quality(lanes_ok)};
	};
	/* Angles are u32 millidegrees in [0, 360000) since UNBAL-v2. */
	const auto angle_of = [&](std::size_t base, bool lanes_ok,
				  std::int64_t rms) {
		return Reading<Millidegrees>{
			static_cast<std::int64_t>(record.word(base + 1)),
			quality(lanes_ok && reference_valid && rms != 0),
			sequence, received_at, window};
	};
	/* Voltage components: zero/positive/negative at words 16/18/20. */
	Reading<MicroVolts> v_seq[3];
	Reading<Millidegrees> v_angle[3];
	for (std::size_t k = 0; k < 3; ++k) {
		const auto [value, grade] = magnitude(16u + k * 2u, v_lanes);
		v_seq[k] = {value, grade, sequence, received_at, window};
		v_angle[k] = angle_of(16u + k * 2u, v_lanes, value);
	}
	values.voltage_zero_sequence = v_seq[0];
	values.voltage_positive_sequence = v_seq[1];
	values.voltage_negative_sequence = v_seq[2];
	values.voltage_zero_angle = v_angle[0];
	values.voltage_positive_angle = v_angle[1];
	values.voltage_negative_angle = v_angle[2];
	/* Current components at words 22/24/26. */
	Reading<MicroAmperes> i_seq[3];
	Reading<Millidegrees> i_angle[3];
	for (std::size_t k = 0; k < 3; ++k) {
		const auto [value, grade] = magnitude(22u + k * 2u, i_lanes);
		i_seq[k] = {value, grade, sequence, received_at, window};
		i_angle[k] = angle_of(22u + k * 2u, i_lanes, value);
	}
	values.current_zero_sequence = i_seq[0];
	values.current_positive_sequence = i_seq[1];
	values.current_negative_sequence = i_seq[2];
	values.current_zero_angle = i_angle[0];
	values.current_positive_angle = i_angle[1];
	values.current_negative_angle = i_angle[2];

	const auto ratio = [&](std::size_t word, bool available) {
		return Reading<RatioMillionths>{
			static_cast<std::int64_t>(record.word(word)),
			quality(available), sequence, received_at, window};
	};
	values.voltage_zero_ratio = ratio(28u, v_lanes && v_ratios_valid);
	values.voltage_unbalance = ratio(29u, v_lanes && v_ratios_valid);
	values.current_zero_ratio = ratio(30u, i_lanes && i_ratios_valid);
	values.current_unbalance = ratio(31u, i_lanes && i_ratios_valid);

	MeterUpdate update{};
	update.period = period;
	update.kind = RecordKind::unbalance;
	update.sequence = sequence;
	update.configuration_generation = record.configuration_generation();
	update.unbalance = values;
	return update;
}

} // namespace

MeterUpdate decode_unbalance_meter_record(const MeterRecord &record,
					  SystemTime received_at)
{
	return decode_unbalance_payload(record, received_at,
					meter_unbalance_format,
					MeasurementPeriod::Basic);
}

MeterUpdate decode_aggregate_unbalance_meter_record(const MeterRecord &record,
						    SystemTime received_at)
{
	return decode_unbalance_payload(record, received_at,
					meter_aggregate_unbalance_format,
					MeasurementPeriod::Cycles150_180);
}

MeterUpdate decode_ten_minute_unbalance_meter_record(
	const MeterRecord &record, SystemTime received_at)
{
	return decode_unbalance_payload(record, received_at,
					meter_ten_minute_unbalance_format,
					MeasurementPeriod::Min10);
}

MeterUpdate decode_two_hour_unbalance_meter_record(
	const MeterRecord &record, SystemTime received_at)
{
	return decode_unbalance_payload(record, received_at,
					meter_two_hour_unbalance_format,
					MeasurementPeriod::Hour2);
}

MeterUpdate decode_periodic_meter_record(const MeterRecord &record,
					 SystemTime received_at)
{
	if (!record.header_valid() ||
	    record.record_format() != meter_periodic_format)
		throw std::invalid_argument("invalid basic record");
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
			"invalid nominal frequency in MTR1 timing word");
	const auto nominal = timing_word.nominal_frequency_hz == 50u
		? NominalFrequency::Hz50
		: NominalFrequency::Hz60;
	const auto sample_count = record.block_sample_count();
	if (sample_count == 0u)
		throw std::invalid_argument(
			"MTR1 block has a zero sample count");
	const auto first_sample_index = record.first_sample_index();
	/*
	 * Zero is not a reachable index. The PL conversion stage issues index 1
	 * for the FIRST accepted frame, and the counter is free-running and
	 * monotonic: never reset by a configuration apply, never stepped for
	 * time synchronization. So no real block can begin at 0.
	 *
	 * This is not defense in depth against a hypothetical regression — it
	 * is an observed hardware fault. Accepting a zero anchors the block's
	 * UTC label at the start of capture, which is hours wrong, and does so
	 * while carrying the sync point's small uncertainty bound and a
	 * Synchronized quality label. Nothing downstream can distinguish that
	 * from a good timestamp, so it must be rejected here.
	 */
	if (first_sample_index == 0u)
		throw std::invalid_argument(
			"MTR1 block has a zero first-sample index");
	if (first_sample_index >
	    std::numeric_limits<std::uint64_t>::max() - sample_count)
		throw std::invalid_argument(
			"MTR1 sample range overflows the 64-bit counter");
	/* A locked block is cycle-defined by construction: exactly the
	 * nominal's cycles-per-block. Fallback blocks are time-defined and
	 * may close any cycle count, including 0 or a partial tail. */
	if (timing_word.cycle_locked && !timing_word.free_run_fallback &&
	    timing_word.cycle_count != cycles_per_basic_block(nominal))
		throw std::invalid_argument(
			"MTR1 cycle-locked block has an impossible cycle count");

	const auto sequence = static_cast<std::uint64_t>(record.sequence());
	/* Word 6 is the ACTUAL sample count of this cycle-defined block. */
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
	 * mirroring the hardened MTR1 rules: the PL emits only complete
	 * 15-block aggregates whose cycle count follows the nominal, so any
	 * other shape is corruption or a future RTL regression and must
	 * never silently decode into a valid aggregate.
	 */
	/*
	 * The producer marks every emitted aggregate complete because only
	 * complete 15-block intervals are ever published. A record that says
	 * otherwise is corruption or an RTL regression, never a partial
	 * result to be salvaged.
	 */
	if (!record.aggregate_status().complete)
		throw std::invalid_argument(
			"MTR2 aggregate is not marked complete");
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
	/*
	 * The first/last basic sequences must describe exactly 15 consecutive
	 * basic blocks. Both accessors return uint32_t, so the subtraction is
	 * modular and a span that wraps 0xFFFFFFFF is accepted unchanged.
	 */
	const auto basic_span =
		static_cast<std::uint32_t>(record.last_basic_sequence() -
					   record.first_basic_sequence());
	if (basic_span != meter::basic_blocks_per_aggregate - 1u)
		throw std::invalid_argument(
			"MTR2 basic sequence span is not 15 consecutive blocks");
	const auto sample_count = record.aggregate_sample_count();
	if (sample_count == 0u)
		throw std::invalid_argument(
			"MTR2 aggregate has a zero sample count");
	const auto first_sample_index = record.aggregate_first_sample_index();
	/* Same unreachable-zero rule as the basic block: the aggregate's first
	 * sample is the first sample of its first contributing block, on the
	 * same free-running counter, so 0 means disturbed provenance. An
	 * aggregate seeded on a block whose index was zeroed inherits it. */
	if (first_sample_index == 0u)
		throw std::invalid_argument(
			"MTR2 aggregate has a zero first-sample index");
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
		record, sequence, received_at, window,
		record.aggregate_status().arithmetic_error, false);

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

MeterUpdate decode_ten_minute_meter_record(const MeterRecord &record,
					    SystemTime received_at)
{
	if (!record.header_valid() ||
	    record.record_format() != meter_ten_minute_format)
		throw std::invalid_argument("invalid ten-minute aggregate record");

	const auto status = record.ten_minute_status();
	if (!status.complete)
		throw std::invalid_argument(
			"ten-minute aggregate is not marked complete");
	const auto composition = record.ten_minute_composition();
	if (composition.nominal_frequency_hz != 50u &&
	    composition.nominal_frequency_hz != 60u)
		throw std::invalid_argument(
			"invalid nominal frequency in ten-minute composition");
	if (composition.basic_block_count == 0u || composition.cycle_count == 0u)
		throw std::invalid_argument(
			"ten-minute aggregate has an empty composition");
	const auto nominal = composition.nominal_frequency_hz == 50u
		? NominalFrequency::Hz50
		: NominalFrequency::Hz60;
	const auto expected_cycles =
		static_cast<std::uint64_t>(composition.basic_block_count) *
		cycles_per_basic_block(nominal);
	if (composition.cycle_count != expected_cycles)
		throw std::invalid_argument(
			"ten-minute aggregate cycle count does not match its blocks");

	const auto first_basic = record.first_basic_sequence();
	const auto last_basic = record.last_basic_sequence();
	const auto basic_span = static_cast<std::uint32_t>(last_basic - first_basic);
	if (basic_span !=
	    static_cast<std::uint32_t>(composition.basic_block_count - 1u))
		throw std::invalid_argument(
			"ten-minute basic sequence span is not consecutive");

	const auto sample_count = record.aggregate_sample_count();
	const auto first_sample = record.aggregate_first_sample_index();
	if (sample_count == 0u || first_sample == 0u)
		throw std::invalid_argument(
			"ten-minute aggregate has an empty sample range");
	if (first_sample >
	    std::numeric_limits<std::uint64_t>::max() -
		static_cast<std::uint64_t>(sample_count - 1u))
		throw std::invalid_argument(
			"ten-minute sample range overflows the 64-bit counter");
	const auto expected_last = first_sample + sample_count - 1u;
	const auto actual_last = record.ten_minute_actual_last_sample_index();
	if (actual_last != expected_last)
		throw std::invalid_argument(
			"ten-minute actual boundary does not match its sample range");
	const auto target = record.ten_minute_target_sample_index();
	if (target > actual_last)
		throw std::invalid_argument(
			"ten-minute target boundary follows its actual boundary");
	const auto overshoot = actual_last - target;
	if (overshoot > std::numeric_limits<std::uint32_t>::max() ||
	    record.ten_minute_overshoot_samples() != overshoot)
		throw std::invalid_argument(
			"ten-minute boundary overshoot is inconsistent");

	const auto sequence = static_cast<std::uint64_t>(record.sequence());
	const auto window = sample_window(sample_count, record.sample_rate_hz());
	const bool invalid_interval = status.contaminated || !status.time_aligned ||
				      !status.boundary_valid;
	MeterUpdate update{};
	update.period = MeasurementPeriod::Min10;
	update.kind = RecordKind::fundamental;
	update.sequence = sequence;
	update.configuration_generation = record.configuration_generation();
	update.fundamental = decode_aggregate_fundamental_values(
		record, sequence, received_at, window, status.arithmetic_error,
		invalid_interval);

	meter::AggregateTiming timing{};
	timing.sequence = sequence;
	timing.configuration_generation = record.configuration_generation();
	timing.first_sample_index = first_sample;
	timing.sample_count = sample_count;
	timing.first_basic_sequence = first_basic;
	timing.last_basic_sequence = last_basic;
	timing.basic_block_count = composition.basic_block_count;
	timing.cycle_count = composition.cycle_count;
	timing.nominal_frequency = nominal;
	timing.arithmetic_error = status.arithmetic_error;
	timing.frequency_valid = false;
	timing.time_aligned = status.time_aligned;
	timing.contaminated = status.contaminated;
	timing.boundary_valid = status.boundary_valid;
	timing.target_sample_index = target;
	timing.overshoot_samples = record.ten_minute_overshoot_samples();
	update.aggregate_timing = timing;
	return update;
}

MeterUpdate decode_two_hour_meter_record(const MeterRecord &record,
					   SystemTime received_at)
{
	if (!record.header_valid() ||
	    record.record_format() != meter_two_hour_format)
		throw std::invalid_argument("invalid two-hour aggregate record");

	const auto status = record.two_hour_status();
	if (!status.complete)
		throw std::invalid_argument(
			"two-hour aggregate is not marked complete");
	const auto composition = record.two_hour_composition();
	if (composition.nominal_frequency_hz != 50u &&
	    composition.nominal_frequency_hz != 60u)
		throw std::invalid_argument(
			"invalid nominal frequency in two-hour composition");
	/* M14 is deliberately the one cascaded aggregate: exactly twelve
	 * complete ten-minute accumulator images enter one result. */
	constexpr std::uint16_t intervals_per_two_hours = 12u;
	if (composition.basic_block_count != intervals_per_two_hours ||
	    composition.cycle_count == 0u)
		throw std::invalid_argument(
			"two-hour aggregate is not built from twelve intervals");
	const auto nominal = composition.nominal_frequency_hz == 50u
		? NominalFrequency::Hz50
		: NominalFrequency::Hz60;

	const auto first_interval = record.first_basic_sequence();
	const auto last_interval = record.last_basic_sequence();
	const auto interval_span =
		static_cast<std::uint32_t>(last_interval - first_interval);
	if (interval_span != intervals_per_two_hours - 1u)
		throw std::invalid_argument(
			"two-hour ten-minute sequence span is not consecutive");

	const auto sample_count = record.aggregate_sample_count();
	const auto first_sample = record.aggregate_first_sample_index();
	if (sample_count == 0u || first_sample == 0u)
		throw std::invalid_argument(
			"two-hour aggregate has an empty sample range");
	if (first_sample >
	    std::numeric_limits<std::uint64_t>::max() -
		static_cast<std::uint64_t>(sample_count - 1u))
		throw std::invalid_argument(
			"two-hour sample range overflows the 64-bit counter");
	const auto expected_last = first_sample + sample_count - 1u;
	const auto actual_last = record.two_hour_actual_last_sample_index();
	if (actual_last != expected_last)
		throw std::invalid_argument(
			"two-hour actual boundary does not match its sample range");
	const auto target = record.two_hour_target_sample_index();
	if (target > actual_last)
		throw std::invalid_argument(
			"two-hour target boundary follows its actual boundary");
	const auto overshoot = actual_last - target;
	if (overshoot > std::numeric_limits<std::uint32_t>::max() ||
	    record.two_hour_overshoot_samples() != overshoot)
		throw std::invalid_argument(
			"two-hour boundary overshoot is inconsistent");

	const auto sequence = static_cast<std::uint64_t>(record.sequence());
	const auto window = sample_window(sample_count, record.sample_rate_hz());
	const bool invalid_interval = status.contaminated || !status.time_aligned ||
				      !status.boundary_valid;
	MeterUpdate update{};
	update.period = MeasurementPeriod::Hour2;
	update.kind = RecordKind::fundamental;
	update.sequence = sequence;
	update.configuration_generation = record.configuration_generation();
	update.fundamental = decode_aggregate_fundamental_values(
		record, sequence, received_at, window, status.arithmetic_error,
		invalid_interval);

	meter::AggregateTiming timing{};
	timing.sequence = sequence;
	timing.configuration_generation = record.configuration_generation();
	timing.first_sample_index = first_sample;
	timing.sample_count = sample_count;
	timing.first_basic_sequence = first_interval;
	timing.last_basic_sequence = last_interval;
	timing.basic_block_count = composition.basic_block_count;
	timing.cycle_count = composition.cycle_count;
	timing.nominal_frequency = nominal;
	timing.arithmetic_error = status.arithmetic_error;
	timing.frequency_valid = false;
	timing.time_aligned = status.time_aligned;
	timing.contaminated = status.contaminated;
	timing.boundary_valid = status.boundary_valid;
	timing.target_sample_index = target;
	timing.overshoot_samples = record.two_hour_overshoot_samples();
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
	result.register_decoder(meter_power_format,
		[](const MeterRecord &record, SystemTime received_at) {
			return decode_power_meter_record(record, received_at);
		});
	result.register_decoder(meter_phasor_format,
		[](const MeterRecord &record, SystemTime received_at) {
			return decode_phasor_meter_record(record, received_at);
		});
	result.register_decoder(meter_unbalance_format,
		[](const MeterRecord &record, SystemTime received_at) {
			return decode_unbalance_meter_record(record,
							     received_at);
		});
	result.register_decoder(meter_aggregate_power_format,
		[](const MeterRecord &record, SystemTime received_at) {
			return decode_aggregate_power_meter_record(
				record, received_at);
		});
	result.register_decoder(meter_aggregate_phasor_format,
		[](const MeterRecord &record, SystemTime received_at) {
			return decode_aggregate_phasor_meter_record(
				record, received_at);
		});
	result.register_decoder(meter_aggregate_unbalance_format,
		[](const MeterRecord &record, SystemTime received_at) {
			return decode_aggregate_unbalance_meter_record(
				record, received_at);
		});
	result.register_decoder(meter_ten_minute_power_format,
		[](const MeterRecord &record, SystemTime received_at) {
			return decode_ten_minute_power_meter_record(record,
							       received_at);
		});
	result.register_decoder(meter_ten_minute_phasor_format,
		[](const MeterRecord &record, SystemTime received_at) {
			return decode_ten_minute_phasor_meter_record(record,
							        received_at);
		});
	result.register_decoder(meter_ten_minute_unbalance_format,
		[](const MeterRecord &record, SystemTime received_at) {
			return decode_ten_minute_unbalance_meter_record(record,
							           received_at);
		});
	result.register_decoder(meter_two_hour_power_format,
		[](const MeterRecord &record, SystemTime received_at) {
			return decode_two_hour_power_meter_record(record,
							  received_at);
		});
	result.register_decoder(meter_two_hour_phasor_format,
		[](const MeterRecord &record, SystemTime received_at) {
			return decode_two_hour_phasor_meter_record(record,
							   received_at);
		});
	result.register_decoder(meter_two_hour_unbalance_format,
		[](const MeterRecord &record, SystemTime received_at) {
			return decode_two_hour_unbalance_meter_record(record,
							     received_at);
		});
	result.register_decoder(meter_periodic_format,
		[](const MeterRecord &record, SystemTime received_at) {
			return decode_periodic_meter_record(record,
							    received_at);
		});
	/* MTR2 aggregates interleave with basic records on the same DMA
	 * stream; the registry routes them by the format word. */
	result.register_decoder(meter_aggregate_format,
		[](const MeterRecord &record, SystemTime received_at) {
			return decode_aggregate_meter_record(record,
							     received_at);
		});
	result.register_decoder(meter_ten_minute_format,
		[](const MeterRecord &record, SystemTime received_at) {
			return decode_ten_minute_meter_record(record, received_at);
		});
	result.register_decoder(meter_two_hour_format,
		[](const MeterRecord &record, SystemTime received_at) {
			return decode_two_hour_meter_record(record, received_at);
		});
	return result;
}


SingleCycleSnapshot decode_single_cycle_record(const MeterRecord &record)
{
	/* Word map: PL contract in MSAP1_PL .../common/include/
	 * measurement_record.hpp (SCYC-v5). */
	SingleCycleSnapshot snapshot;
	snapshot.sequence = record.word(3);
	snapshot.configuration_generation = record.word(4);
	snapshot.sample_count = record.word(6);
	snapshot.valid_mask = record.word(7);
	snapshot.status = record.word(8);
	snapshot.first_sample =
		record.word(9) | (std::uint64_t{record.word(10)} << 32);
	snapshot.nominal_hz = record.word(13) & 0xffu;
	snapshot.flags = (record.word(13) >> 16) & 0x7u;
	snapshot.cycle_sequence = record.word(14);
	snapshot.last_sample =
		record.word(16) | (std::uint64_t{record.word(17)} << 32);
	snapshot.processing_tick =
		record.word(18) | (std::uint64_t{record.word(19)} << 32);
	snapshot.frequency_millihz = record.word(20);
	snapshot.frequency_status = record.word(21);
	for (std::size_t lane = 0; lane < snapshot.rms_micro_units.size();
	     ++lane) {
		const std::size_t base = 24 + lane * 2;
		snapshot.rms_micro_units[lane] =
			record.word(base) |
			(std::uint64_t{record.word(base + 1)} << 32);
	}
	for (std::size_t pair = 0; pair < snapshot.vll_rms_micro_units.size();
	     ++pair) {
		const std::size_t base = 38 + pair * 2;
		snapshot.vll_rms_micro_units[pair] =
			record.word(base) |
			(std::uint64_t{record.word(base + 1)} << 32);
	}
	for (std::size_t phase = 0;
	     phase < snapshot.active_power_picowatts.size(); ++phase) {
		const std::size_t base = 44 + phase * 2;
		snapshot.active_power_picowatts[phase] =
			static_cast<std::int64_t>(
				record.word(base) |
				(std::uint64_t{record.word(base + 1)} << 32));
	}
	for (std::size_t lane = 0;
	     lane < snapshot.fundamental_rms_micro_units.size(); ++lane) {
		const std::size_t base = 50 + lane * 2;
		snapshot.fundamental_rms_micro_units[lane] =
			record.word(base) |
			(std::uint64_t{record.word(base + 1)} << 32);
	}
	return snapshot;
}

namespace {

/* Lane bits in the record's valid mask (PL metering_types.hpp lane
 * numbering): voltages Va/Vb/Vc are lanes 6/5/4, currents Ia/Ib/Ic 0/1/2. */
constexpr std::uint8_t pq_voltage_lane_bit[3] = {0x40u, 0x20u, 0x10u};
constexpr std::uint8_t pq_current_lane_bit[3] = {0x01u, 0x02u, 0x04u};

} // namespace

PowerQualitySnapshot decode_pq_event_record(const MeterRecord &record)
{
	/* Word map: PL contract in MSAP1_PL .../common/include/
	 * measurement_record.hpp (PQEVT-v1). */
	if (record.record_format() != meter_pq_event_format)
		throw std::invalid_argument("invalid power-quality record");
	const auto kind_word = record.word(13);
	const auto kind = kind_word & 0xffu;
	const auto event_type = (kind_word >> 8) & 0xffu;
	if (kind > 2u || event_type > 3u)
		throw std::invalid_argument(
			"power-quality record has an unknown kind or event type");
	const auto sample_count = record.block_sample_count();
	if (sample_count == 0u)
		throw std::invalid_argument(
			"power-quality record has a zero sample count");

	PowerQualitySnapshot snapshot;
	snapshot.sequence = record.sequence();
	snapshot.configuration_generation = record.configuration_generation();
	snapshot.sample_rate_hz = record.sample_rate_hz();
	snapshot.sample_count = sample_count;
	snapshot.valid_mask = record.valid_mask();
	snapshot.status = record.status();
	snapshot.first_sample = record.first_sample_index();
	snapshot.last_sample = record.unsigned64(14);
	if (snapshot.last_sample < snapshot.first_sample)
		throw std::invalid_argument(
			"power-quality record spans a negative sample range");

	auto &values = snapshot.values;
	values.kind = static_cast<PowerQualityRecordKind>(kind);
	values.event_type = static_cast<PowerQualityEventType>(event_type);
	values.affected_phases = static_cast<std::uint8_t>((kind_word >> 16) & 0x7u);
	values.cycle_locked = (kind_word & (1u << 24)) != 0u;
	values.synthetic_half_cycle = (kind_word & (1u << 25)) != 0u;
	values.armed = (kind_word & (1u << 26)) != 0u;
	values.event_sequence = record.word(28);
	values.duration_samples = record.unsigned64(29);
	values.half_cycle_updates = record.word(31);
	values.reference_micro_volts = record.word(32);
	values.sag_threshold_e4 = record.word(33);
	values.swell_threshold_e4 = record.word(34);
	values.interruption_threshold_e4 = record.word(35);
	values.hysteresis_e4 = record.word(36);

	const auto sequence = static_cast<std::uint64_t>(snapshot.sequence);
	const auto received_at = std::chrono::system_clock::now();
	const auto window = sample_window(sample_count, snapshot.sample_rate_hz);
	/* A lane outside the configured valid mask was never measured; a
	 * saturated accumulator makes every lane's root untrustworthy. */
	const auto quality = [&](std::uint8_t lane_bit) {
		if ((snapshot.valid_mask & lane_bit) == 0u)
			return MeasurementQuality::unavailable;
		return snapshot.arithmetic_error()
			       ? MeasurementQuality::arithmetic_error
			       : MeasurementQuality::valid;
	};
	const auto voltage = [&](std::size_t word, std::size_t phase) {
		return Reading<MicroVolts>{
			static_cast<std::int64_t>(record.word(word)),
			quality(pq_voltage_lane_bit[phase]), sequence,
			received_at, window};
	};
	values.voltage = {voltage(16, 0), voltage(17, 1), voltage(18, 2)};
	values.voltage_minimum = {voltage(19, 0), voltage(20, 1), voltage(21, 2)};
	values.voltage_maximum = {voltage(22, 0), voltage(23, 1), voltage(24, 2)};
	values.current = {
		Reading<MicroAmperes>{static_cast<std::int64_t>(record.word(25)),
				      quality(pq_current_lane_bit[0]), sequence,
				      received_at, window},
		Reading<MicroAmperes>{static_cast<std::int64_t>(record.word(26)),
				      quality(pq_current_lane_bit[1]), sequence,
				      received_at, window},
		Reading<MicroAmperes>{static_cast<std::int64_t>(record.word(27)),
				      quality(pq_current_lane_bit[2]), sequence,
				      received_at, window}};
	return snapshot;
}

} // namespace msap1
