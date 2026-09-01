#include "msap1/meter/meter_data.hpp"
#include "msap1/meter/energy_demand.hpp"

#include <algorithm>
#include <condition_variable>
#include <limits>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

#include <openssl/sha.h>

namespace msap1 {
namespace {

std::size_t period_index(MeasurementPeriod period)
{
	const auto index = static_cast<std::size_t>(period);
	if (index >= 8)
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
	    period != MeasurementPeriod::Hour2 &&
	    period != MeasurementPeriod::Min10Live &&
	    period != MeasurementPeriod::Hour2Live)
		return {};
	const auto status = (period == MeasurementPeriod::Hour2 ||
			     period == MeasurementPeriod::Hour2Live)
		? record.two_hour_status()
		: record.ten_minute_status();
	const bool preview = period == MeasurementPeriod::Min10Live ||
		period == MeasurementPeriod::Hour2Live;
	const bool preview_contract_invalid = preview &&
		(status.complete || !status.open_interval || !status.non_normative);
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
		incomplete || preview_contract_invalid || !status.time_aligned ||
			status.contaminated ||
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

PowerQualityEventUuid stable_power_quality_event_uuid(
	const PowerQualityEventId &id)
{
	std::array<std::byte, 16> source{};
	for (unsigned byte = 0; byte < 8u; ++byte) {
		source[byte] = static_cast<std::byte>(
			(id.session >> (byte * 8u)) & 0xffu);
		source[8u + byte] = static_cast<std::byte>(
			(id.counter >> (byte * 8u)) & 0xffu);
	}
	std::array<std::byte, SHA256_DIGEST_LENGTH> digest{};
	if (SHA256(reinterpret_cast<const unsigned char *>(source.data()),
			source.size(),
			reinterpret_cast<unsigned char *>(digest.data())) == nullptr)
		throw std::runtime_error("power-quality event UUID hash failed");
	PowerQualityEventUuid result{};
	std::copy_n(digest.begin(), result.size(), result.begin());
	result[6] = static_cast<std::byte>(
		(std::to_integer<std::uint8_t>(result[6]) & 0x0fu) | 0x50u);
	result[8] = static_cast<std::byte>(
		(std::to_integer<std::uint8_t>(result[8]) & 0x3fu) | 0x80u);
	return result;
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
	if (update.frequency_10s)
		slot->frequency_10s = update.frequency_10s;
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
 * Channel and frequency decoding for the 10/12-cycle basic record format.
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
 * 150/180-cycle aggregate fundamental decoding. Channel order and micro-unit
 * encoding match the basic record; only the word layout differs — two words
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
	 * The aggregate frequency field is informational only: it is the mean of
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

MeterUpdate decode_ten_minute_open_power_meter_record(
	const MeterRecord &record, SystemTime received_at)
{
	return decode_power_payload(record, received_at,
				    meter_ten_minute_open_power_format,
				    MeasurementPeriod::Min10Live);
}

MeterUpdate decode_two_hour_open_power_meter_record(
	const MeterRecord &record, SystemTime received_at)
{
	return decode_power_payload(record, received_at,
				    meter_two_hour_open_power_format,
				    MeasurementPeriod::Hour2Live);
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

MeterUpdate decode_ten_minute_open_phasor_meter_record(
	const MeterRecord &record, SystemTime received_at)
{
	return decode_phasor_payload(record, received_at,
				     meter_ten_minute_open_phasor_format,
				     MeasurementPeriod::Min10Live);
}

MeterUpdate decode_two_hour_open_phasor_meter_record(
	const MeterRecord &record, SystemTime received_at)
{
	return decode_phasor_payload(record, received_at,
				     meter_two_hour_open_phasor_format,
				     MeasurementPeriod::Hour2Live);
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

MeterUpdate decode_ten_minute_open_unbalance_meter_record(
	const MeterRecord &record, SystemTime received_at)
{
	return decode_unbalance_payload(record, received_at,
					meter_ten_minute_open_unbalance_format,
					MeasurementPeriod::Min10Live);
}

MeterUpdate decode_two_hour_open_unbalance_meter_record(
	const MeterRecord &record, SystemTime received_at)
{
	return decode_unbalance_payload(record, received_at,
					meter_two_hour_open_unbalance_format,
					MeasurementPeriod::Hour2Live);
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
			"invalid nominal frequency in 10/12-cycle basic timing word");
	const auto nominal = timing_word.nominal_frequency_hz == 50u
		? NominalFrequency::Hz50
		: NominalFrequency::Hz60;
	const auto sample_count = record.block_sample_count();
	if (sample_count == 0u)
		throw std::invalid_argument(
			"10/12-cycle basic interval has a zero sample count");
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
			"10/12-cycle basic interval has a zero first-sample index");
	if (first_sample_index >
	    std::numeric_limits<std::uint64_t>::max() - sample_count)
		throw std::invalid_argument(
			"10/12-cycle basic sample range overflows the 64-bit counter");
	/* A locked block is cycle-defined by construction: exactly the
	 * nominal's cycles-per-block. Fallback blocks are time-defined and
	 * may close any cycle count, including 0 or a partial tail. */
	if (timing_word.cycle_locked && !timing_word.free_run_fallback &&
	    timing_word.cycle_count != cycles_per_basic_block(nominal))
		throw std::invalid_argument(
			"10/12-cycle basic interval has an impossible cycle count");

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
	timing.sample_rate_hz = record.sample_rate_hz();
	timing.cycle_count = timing_word.cycle_count;
	timing.nominal_frequency = nominal;
	timing.cycle_locked = timing_word.cycle_locked;
	timing.free_run_fallback = timing_word.free_run_fallback;
	timing.first_block_after_apply = timing_word.first_block_after_apply;
	timing.utc_resynchronized = timing_word.utc_resynchronized;
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
		throw std::invalid_argument(
			"invalid 150/180-cycle aggregate record");
	/*
	 * Validate the aggregation identity before building anything,
	 * mirroring the hardened 10/12-cycle basic rules: the producer emits only
	 * complete 15-block aggregates whose cycle count follows the nominal, so any
	 * other shape is corruption or a future RTL regression and must
	 * never silently decode into a valid aggregate.
	 */
	/*
	 * The producer marks every emitted aggregate complete because only
	 * complete 15-block intervals are ever published. A record that says
	 * otherwise is corruption or an RTL regression, never a partial
	 * result to be salvaged.
	 */
	const auto status = record.aggregate_status();
	if (!status.complete)
		throw std::invalid_argument(
			"150/180-cycle aggregate is not marked complete");
	const auto composition = record.aggregate_composition();
	if (composition.nominal_frequency_hz != 50u &&
	    composition.nominal_frequency_hz != 60u)
		throw std::invalid_argument(
			"invalid nominal frequency in 150/180-cycle aggregate composition word");
	const auto nominal = composition.nominal_frequency_hz == 50u
		? NominalFrequency::Hz50
		: NominalFrequency::Hz60;
	if (composition.basic_block_count != meter::basic_blocks_per_aggregate)
		throw std::invalid_argument(
			"150/180-cycle aggregate is not built from exactly 15 basic intervals");
	if (composition.cycle_count != cycles_per_aggregate(nominal))
		throw std::invalid_argument(
			"150/180-cycle aggregate cycle count does not match its nominal frequency");
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
			"150/180-cycle aggregate does not span 15 consecutive basic intervals");
	const auto sample_count = record.aggregate_sample_count();
	if (sample_count == 0u)
		throw std::invalid_argument(
			"150/180-cycle aggregate has a zero sample count");
	const auto first_sample_index = record.aggregate_first_sample_index();
	/* Same unreachable-zero rule as the basic block: the aggregate's first
	 * sample is the first sample of its first contributing block, on the
	 * same free-running counter, so 0 means disturbed provenance. An
	 * aggregate seeded on a block whose index was zeroed inherits it. */
	if (first_sample_index == 0u)
		throw std::invalid_argument(
			"150/180-cycle aggregate has a zero first-sample index");
	if (first_sample_index >
	    std::numeric_limits<std::uint64_t>::max() - sample_count)
		throw std::invalid_argument(
			"150/180-cycle aggregate sample range overflows the 64-bit counter");
	if (status.utc_overlap && status.utc_resynchronized)
		throw std::invalid_argument(
			"150/180-cycle aggregate has conflicting UTC provenance");
	const auto expected_last =
		first_sample_index + static_cast<std::uint64_t>(sample_count) - 1u;
	const auto actual_last = record.aggregate_last_sample_index();
	if (status.utc_overlap) {
		/* A continuing aggregate overlaps the new synchronized aggregate
		 * because both include the synchronized Basic interval. When UTC
		 * lands inside an open Basic interval, the continuing aggregate's
		 * summed contributions overlap internally and actual_last is earlier
		 * than expected_last. At an exact Basic boundary its contributors are
		 * contiguous and actual_last equals expected_last. Both geometries are
		 * valid; only a range beyond the contribution span is impossible. */
		if (actual_last < first_sample_index || actual_last > expected_last)
			throw std::invalid_argument(
				"150/180-cycle UTC-overlap range exceeds its contribution span");
	} else if (actual_last != expected_last) {
		throw std::invalid_argument(
			"150/180-cycle aggregate sample range is discontinuous");
	}

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

	meter::AggregateTiming timing{};
	timing.sequence = sequence;
	timing.configuration_generation = record.configuration_generation();
	timing.first_sample_index = first_sample_index;
	timing.last_sample_index = actual_last;
	timing.sample_count = sample_count;
	timing.sample_rate_hz = record.sample_rate_hz();
	timing.first_basic_sequence = record.first_basic_sequence();
	timing.last_basic_sequence = record.last_basic_sequence();
	timing.basic_block_count = composition.basic_block_count;
	timing.cycle_count = composition.cycle_count;
	timing.nominal_frequency = nominal;
	timing.arithmetic_error = status.arithmetic_error;
	timing.frequency_valid = status.frequency_valid;
	timing.utc_overlap = status.utc_overlap;
	timing.utc_resynchronized = status.utc_resynchronized;
	/* TimeQuality/utc_start/utc_uncertainty_ns are stamped by the caller:
	 * UTC state lives in the APU MeasurementTimebase, never in the PL
	 * record. */
	update.aggregate_timing = timing;
	return update;
}

namespace {

MeterUpdate decode_open_interval_meter_record(
	const MeterRecord &record, SystemTime received_at,
	std::uint32_t expected_format, MeasurementPeriod period)
{
	if (!record.header_valid() || record.record_format() != expected_format)
		throw std::invalid_argument("invalid open aggregate record");
	const auto status = record.ten_minute_status();
	if (status.complete || !status.open_interval || !status.non_normative)
		throw std::invalid_argument(
			"open aggregate lacks its non-normative interval markers");

	const auto composition = record.ten_minute_composition();
	if ((composition.nominal_frequency_hz != 50u &&
	     composition.nominal_frequency_hz != 60u) ||
	    composition.basic_block_count == 0u || composition.cycle_count == 0u)
		throw std::invalid_argument("open aggregate has an invalid composition");
	const auto nominal = composition.nominal_frequency_hz == 50u
		? NominalFrequency::Hz50
		: NominalFrequency::Hz60;
	const auto first_sequence = record.first_basic_sequence();
	const auto last_sequence = record.last_basic_sequence();
	if (static_cast<std::uint32_t>(last_sequence - first_sequence) !=
	    static_cast<std::uint32_t>(composition.basic_block_count - 1u))
		throw std::invalid_argument(
			"open aggregate source sequence span is not consecutive");

	const auto sample_count = record.aggregate_sample_count();
	const auto first_sample = record.aggregate_first_sample_index();
	if (sample_count == 0u || first_sample == 0u ||
	    first_sample > std::numeric_limits<std::uint64_t>::max() -
			   static_cast<std::uint64_t>(sample_count - 1u))
		throw std::invalid_argument("open aggregate has an invalid sample range");
	const auto actual_last = record.ten_minute_actual_last_sample_index();
	if (actual_last != first_sample + sample_count - 1u)
		throw std::invalid_argument(
			"open aggregate last sample does not match its range");
	const auto expected_end = record.ten_minute_target_sample_index();
	if (expected_end < actual_last || record.ten_minute_overshoot_samples() != 0u)
		throw std::invalid_argument(
			"open aggregate expected end or overshoot is inconsistent");

	const auto sequence = static_cast<std::uint64_t>(record.sequence());
	const auto window = sample_window(sample_count, record.sample_rate_hz());
	const bool invalid_interval = status.contaminated || !status.time_aligned ||
				      !status.boundary_valid;
	MeterUpdate update{};
	update.period = period;
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
	timing.last_sample_index = actual_last;
	timing.sample_count = sample_count;
	timing.sample_rate_hz = record.sample_rate_hz();
	timing.first_basic_sequence = first_sequence;
	timing.last_basic_sequence = last_sequence;
	timing.basic_block_count = composition.basic_block_count;
	timing.cycle_count = composition.cycle_count;
	timing.nominal_frequency = nominal;
	timing.arithmetic_error = status.arithmetic_error;
	timing.frequency_valid = false;
	timing.time_aligned = status.time_aligned;
	timing.contaminated = status.contaminated;
	timing.boundary_valid = status.boundary_valid;
	/* For an open record this is the expected interval end, not a boundary
	 * already crossed. The current end is derivable from first+count. */
	timing.target_sample_index = expected_end;
	timing.overshoot_samples = 0u;
	update.aggregate_timing = timing;
	return update;
}

} // namespace

MeterUpdate decode_ten_minute_open_meter_record(const MeterRecord &record,
						 SystemTime received_at)
{
	return decode_open_interval_meter_record(
		record, received_at, meter_ten_minute_open_format,
		MeasurementPeriod::Min10Live);
}

MeterUpdate decode_two_hour_open_meter_record(const MeterRecord &record,
					      SystemTime received_at)
{
	return decode_open_interval_meter_record(
		record, received_at, meter_two_hour_open_format,
		MeasurementPeriod::Hour2Live);
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
	timing.last_sample_index = actual_last;
	timing.sample_count = sample_count;
	timing.sample_rate_hz = record.sample_rate_hz();
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
	timing.last_sample_index = actual_last;
	timing.sample_count = sample_count;
	timing.sample_rate_hz = record.sample_rate_hz();
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

namespace {

std::uint64_t rounded_divide_ties_to_even(std::uint64_t numerator,
	std::uint64_t denominator)
{
	if (denominator == 0u)
		throw std::invalid_argument(
			"frequency 10-second result has a zero duration");
	const auto quotient = numerator / denominator;
	const auto remainder = numerator % denominator;
	const auto complement = denominator - remainder;
	return quotient + static_cast<std::uint64_t>(
		remainder > complement ||
		(remainder == complement && (quotient & 1u) != 0u));
}

std::uint32_t expected_frequency_10s_status(const MeterRecord &record,
	std::uint32_t reasons)
{
	const auto source = record.word(meter_frequency_10s_source_status_word);
	std::uint32_t status = 0u;
	if ((reasons & meter_frequency_10s_reason_arithmetic) != 0u)
		status |= meter_frequency_10s_status_arithmetic_error;
	if (reasons == 0u)
		status |= meter_frequency_10s_status_result_valid;
	if ((source & (1u << 0u)) != 0u)
		status |= meter_frequency_10s_status_time_aligned;
	if ((source & (1u << 10u)) != 0u)
		status |= meter_frequency_10s_status_profile_supported;
	if ((source & (1u << 1u)) != 0u)
		status |= meter_frequency_10s_status_time_synchronized;
	if ((source & (1u << 3u)) != 0u)
		status |= meter_frequency_10s_status_filter_ready;
	if ((source & (1u << 4u)) != 0u)
		status |= meter_frequency_10s_status_reference_valid;
	if ((source & ((1u << 5u) | (1u << 8u))) != 0u)
		status |= meter_frequency_10s_status_discontinuity;
	if ((source & (1u << 6u)) != 0u)
		status |= meter_frequency_10s_status_crossing_overflow;
	if ((source & (1u << 7u)) != 0u ||
	    record.word(meter_frequency_10s_observer_drop_word) != 0u)
		status |= meter_frequency_10s_status_observer_drop;
	if ((reasons & meter_frequency_10s_reason_insufficient_crossings) != 0u)
		status |= meter_frequency_10s_status_insufficient_crossings;
	if ((reasons & (meter_frequency_10s_reason_out_of_range |
			meter_frequency_10s_reason_cycle_geometry)) != 0u)
		status |= meter_frequency_10s_status_out_of_range;
	if ((reasons & meter_frequency_10s_reason_transport_gap) != 0u)
		status |= meter_frequency_10s_status_transport_gap;
	if ((source & (1u << 9u)) != 0u)
		status |= meter_frequency_10s_status_calibration_valid;
	if ((source & (1u << 2u)) != 0u)
		status |= meter_frequency_10s_status_sample_rate_valid;
	if ((source & (1u << 8u)) != 0u)
		status |= meter_frequency_10s_status_resynchronized;
	return status;
}

} // namespace

MeterUpdate decode_frequency_10s_meter_record(const MeterRecord &record,
	SystemTime received_at)
{
	(void)received_at;
	if (!record.header_valid() ||
	    record.record_format() != meter_frequency_10s_format)
		throw std::invalid_argument("invalid frequency 10-second record");
	if (record.word(12) != 0u)
		throw std::invalid_argument(
			"frequency 10-second reserved envelope word is nonzero");
	for (std::size_t word = 42u; word < meter_record_word_count; ++word)
		if (record.word(word) != 0u)
			throw std::invalid_argument(
				"frequency 10-second reserved tail is nonzero");

	const auto nominal_hz = record.frequency_10s_nominal_hz();
	if (nominal_hz != 50u && nominal_hz != 60u)
		throw std::invalid_argument(
			"frequency 10-second nominal frequency is invalid");
	if (record.sample_rate_hz() == 0u)
		throw std::invalid_argument(
			"frequency 10-second sample rate is zero");
	const auto first_sample = record.first_sample_index();
	const auto end_sample = record.frequency_10s_end_sample_index();
	if (end_sample <= first_sample)
		throw std::invalid_argument(
			"frequency 10-second sample interval is empty or reversed");
	const auto sample_span = end_sample - first_sample;
	if (sample_span > std::numeric_limits<std::uint32_t>::max() ||
	    record.block_sample_count() != sample_span ||
	    record.frequency_10s_last_sample_index() != end_sample - 1u)
		throw std::invalid_argument(
			"frequency 10-second sample anchors disagree");

	const auto utc_start = record.frequency_10s_utc_start_nanoseconds();
	const auto utc_end = record.frequency_10s_utc_end_nanoseconds();
	if (utc_end <= utc_start || utc_end - utc_start != 10'000'000'000ull ||
	    utc_end > static_cast<std::uint64_t>(
		std::numeric_limits<std::int64_t>::max()))
		throw std::invalid_argument(
			"frequency 10-second UTC interval is not exactly ten seconds");
	if (record.word(meter_frequency_10s_source_sequence_word) !=
	    record.sequence())
		throw std::invalid_argument(
			"frequency 10-second source sequence disagrees");
	if (record.emit_drops() !=
	    record.word(meter_frequency_10s_observer_drop_word))
		throw std::invalid_argument(
			"frequency 10-second observer drop counters disagree");

	const auto source_status =
		record.word(meter_frequency_10s_source_status_word);
	const auto reasons = record.word(meter_frequency_10s_reason_word);
	const auto status = record.status();
	if ((source_status & ~meter_frequency_10s_source_status_mask) != 0u ||
	    (reasons & ~meter_frequency_10s_reason_mask) != 0u ||
	    (status & ~meter_frequency_10s_status_mask) != 0u ||
	    (record.word(meter_frequency_10s_guard_flags_word) &
	     ~meter_frequency_10s_guard_flags_mask) != 0u)
		throw std::invalid_argument(
			"frequency 10-second flags use reserved bits");
	if (status != expected_frequency_10s_status(record, reasons))
		throw std::invalid_argument(
			"frequency 10-second status and reasons disagree");

	std::uint32_t mandatory_reasons = 0u;
	if (record.sample_rate_hz() != 128000u ||
	    record.frequency_10s_reference_channel() != 6u ||
	    record.frequency_10s_filter_profile() != 1u ||
	    record.frequency_10s_calibration_profile() != 1u ||
	    (source_status & (1u << 10u)) == 0u)
		mandatory_reasons |=
			meter_frequency_10s_reason_unsupported_profile;
	if ((source_status & (1u << 0u)) == 0u)
		mandatory_reasons |= meter_frequency_10s_reason_boundary_invalid;
	if ((source_status & (1u << 1u)) == 0u)
		mandatory_reasons |= meter_frequency_10s_reason_time_unsynchronized;
	if ((source_status & (1u << 2u)) == 0u)
		mandatory_reasons |= meter_frequency_10s_reason_sample_rate_invalid;
	if ((source_status & (1u << 3u)) == 0u)
		mandatory_reasons |= meter_frequency_10s_reason_filter_warmup;
	if ((source_status & (1u << 4u)) == 0u)
		mandatory_reasons |= meter_frequency_10s_reason_reference_invalid;
	if ((source_status & (1u << 9u)) == 0u)
		mandatory_reasons |= meter_frequency_10s_reason_calibration_invalid;
	if ((source_status & ((1u << 5u) | (1u << 8u))) != 0u)
		mandatory_reasons |= meter_frequency_10s_reason_discontinuity;
	if ((source_status & (1u << 6u)) != 0u)
		mandatory_reasons |= meter_frequency_10s_reason_crossing_overflow;
	if ((source_status & (1u << 7u)) != 0u || record.emit_drops() != 0u)
		mandatory_reasons |= meter_frequency_10s_reason_observer_drop;
	if (record.frequency_10s_utc_uncertainty_nanoseconds() > 1'000'000ull)
		mandatory_reasons |= meter_frequency_10s_reason_time_uncertainty;
	const auto measured_rate =
		record.word(meter_frequency_10s_measured_rate_word);
	const auto expected_millisamples =
		static_cast<std::uint64_t>(measured_rate) * 10u;
	const auto actual_millisamples = sample_span * 1000u;
	const auto span_error = expected_millisamples > actual_millisamples
		? expected_millisamples - actual_millisamples
		: actual_millisamples - expected_millisamples;
	if (span_error > 2000u)
		mandatory_reasons |= meter_frequency_10s_reason_time_geometry;
	if ((reasons & mandatory_reasons) != mandatory_reasons)
		throw std::invalid_argument(
			"frequency 10-second record omits a mandatory rejection reason");

	const auto observed =
		record.word(meter_frequency_10s_observed_crossings_word);
	const auto included =
		record.word(meter_frequency_10s_included_crossings_word);
	const auto cycles = record.word(meter_frequency_10s_cycle_count_word);
	const auto rejected =
		record.word(meter_frequency_10s_rejected_cycles_word);
	const auto duration =
		record.unsigned64(meter_frequency_10s_duration_q16_word);
	const auto first_crossing =
		record.signed64(meter_frequency_10s_first_crossing_q16_word);
	const auto last_crossing =
		record.signed64(meter_frequency_10s_last_crossing_q16_word);
	const auto interval_q16 = sample_span << 16u;
	if (observed > meter_frequency_10s_max_observed_crossings ||
	    included > observed || cycles != (included == 0u ? 0u : included - 1u) ||
	    rejected > cycles)
		throw std::invalid_argument(
			"frequency 10-second crossing counts disagree");
	if (included == 0u) {
		if (first_crossing != 0 || last_crossing != 0 || duration != 0u)
			throw std::invalid_argument(
				"frequency 10-second empty crossing geometry is nonzero");
	} else if (first_crossing < 0 || last_crossing < first_crossing ||
		   static_cast<std::uint64_t>(last_crossing) > interval_q16 ||
		   duration != (included >= 2u
			? static_cast<std::uint64_t>(last_crossing - first_crossing)
			: 0u)) {
		throw std::invalid_argument(
			"frequency 10-second crossing geometry is invalid");
	}
	const bool insufficient = cycles == 0u || duration == 0u;
	if (((reasons & meter_frequency_10s_reason_insufficient_crossings) != 0u) !=
	    insufficient ||
	    ((reasons & meter_frequency_10s_reason_cycle_geometry) != 0u) !=
		(rejected != 0u))
		throw std::invalid_argument(
			"frequency 10-second geometry reasons disagree");

	std::uint64_t calculated_frequency = 0u;
	if (!insufficient) {
		const auto scaled_rate = static_cast<std::uint64_t>(measured_rate)
			<< 16u;
		if (scaled_rate > std::numeric_limits<std::uint64_t>::max() /
				cycles)
			throw std::invalid_argument(
				"frequency 10-second arithmetic overflows");
		calculated_frequency = rounded_divide_ties_to_even(
			scaled_rate * cycles, duration);
	}
	const auto minimum = nominal_hz == 50u ? 42500u : 51000u;
	const auto maximum = nominal_hz == 50u ? 57500u : 69000u;
	const bool out_of_range = calculated_frequency != 0u &&
		(calculated_frequency < minimum || calculated_frequency > maximum);
	if (((reasons & meter_frequency_10s_reason_out_of_range) != 0u) !=
	    out_of_range)
		throw std::invalid_argument(
			"frequency 10-second range reason disagrees");
	if ((reasons & meter_frequency_10s_reason_arithmetic) != 0u)
		throw std::invalid_argument(
			"frequency 10-second impossible arithmetic reason");

	const bool valid = reasons == 0u;
	const auto published_frequency =
		record.word(meter_frequency_10s_value_word);
	if (record.valid_mask() != (valid ? (1u << 6u) : 0u) ||
	    (valid && published_frequency != calculated_frequency) ||
	    (!valid && published_frequency != 0u))
		throw std::invalid_argument(
			"frequency 10-second result validity disagrees");

	const auto quality = valid
		? MeasurementQuality::valid
		: (reasons & meter_frequency_10s_reason_arithmetic) != 0u
			? MeasurementQuality::arithmetic_error
		: (reasons & (meter_frequency_10s_reason_out_of_range |
				meter_frequency_10s_reason_cycle_geometry)) != 0u
			? MeasurementQuality::out_of_range
			: MeasurementQuality::invalid;
	const auto measured_at = SystemTime{
		std::chrono::nanoseconds{static_cast<std::int64_t>(utc_end)}};
	const SampleWindow window{record.block_sample_count(),
		std::chrono::seconds{10}};
	FundamentalValues values{};
	values.frequency = {static_cast<std::int64_t>(published_frequency),
		quality, record.sequence(), measured_at, window};

	MeterUpdate update{};
	update.period = MeasurementPeriod::Seconds10;
	update.kind = RecordKind::frequency_10s;
	update.sequence = record.sequence();
	update.configuration_generation = record.configuration_generation();
	update.fundamental = values;
	meter::BlockTiming timing{};
	timing.sequence = record.sequence();
	timing.configuration_generation = record.configuration_generation();
	timing.first_sample_index = first_sample;
	timing.sample_count = record.block_sample_count();
	timing.sample_rate_hz = record.sample_rate_hz();
	timing.cycle_count = static_cast<std::uint16_t>(cycles);
	timing.nominal_frequency = nominal_hz == 50u
		? NominalFrequency::Hz50 : NominalFrequency::Hz60;
	timing.time_quality =
		(status & meter_frequency_10s_status_time_synchronized) != 0u &&
		record.frequency_10s_utc_uncertainty_nanoseconds() <= 1'000'000ull
			? TimeQuality::Synchronized : TimeQuality::Unsynchronized;
	timing.utc_start = SystemTime{
		std::chrono::nanoseconds{static_cast<std::int64_t>(utc_start)}};
	timing.utc_uncertainty_ns =
		record.frequency_10s_utc_uncertainty_nanoseconds();
	update.timing = timing;
	update.frequency_10s = Frequency10sMetadata{
		.interval_end_sample_index = end_sample,
		.utc_start_nanoseconds = utc_start,
		.utc_end_nanoseconds = utc_end,
		.utc_uncertainty_nanoseconds =
			record.frequency_10s_utc_uncertainty_nanoseconds(),
		.measured_sample_rate_millihz = measured_rate,
		.source_sequence =
			record.word(meter_frequency_10s_source_sequence_word),
		.boundary_generation =
			record.word(meter_frequency_10s_boundary_generation_word),
		.source_status = source_status,
		.status = status,
		.reasons = reasons,
		.observer_drop_count =
			record.word(meter_frequency_10s_observer_drop_word),
		.guard_flags = static_cast<std::uint8_t>(
			record.word(meter_frequency_10s_guard_flags_word)),
		.observed_crossings = observed,
		.included_crossings = included,
		.rejected_cycles = rejected,
		.duration_q16_samples = duration,
		.first_crossing_q16_samples = first_crossing,
		.last_crossing_q16_samples = last_crossing,
		.nominal_frequency_hz = nominal_hz,
		.reference_channel = record.frequency_10s_reference_channel(),
		.filter_profile = record.frequency_10s_filter_profile(),
		.calibration_profile =
			record.frequency_10s_calibration_profile(),
	};
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
	result.register_decoder(meter_frequency_10s_format,
		[](const MeterRecord &record, SystemTime received_at) {
			return decode_frequency_10s_meter_record(record, received_at);
		});
	result.register_decoder(meter_demand_format,
		[](const MeterRecord &record, SystemTime received_at) {
			return decode_demand_meter_record(record, received_at);
		});
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
	result.register_decoder(meter_ten_minute_open_power_format,
		[](const MeterRecord &record, SystemTime received_at) {
			return decode_ten_minute_open_power_meter_record(record,
							    received_at);
		});
	result.register_decoder(meter_ten_minute_open_phasor_format,
		[](const MeterRecord &record, SystemTime received_at) {
			return decode_ten_minute_open_phasor_meter_record(record,
							     received_at);
		});
	result.register_decoder(meter_ten_minute_open_unbalance_format,
		[](const MeterRecord &record, SystemTime received_at) {
			return decode_ten_minute_open_unbalance_meter_record(
				record, received_at);
		});
	result.register_decoder(meter_two_hour_open_power_format,
		[](const MeterRecord &record, SystemTime received_at) {
			return decode_two_hour_open_power_meter_record(record,
							  received_at);
		});
	result.register_decoder(meter_two_hour_open_phasor_format,
		[](const MeterRecord &record, SystemTime received_at) {
			return decode_two_hour_open_phasor_meter_record(record,
							   received_at);
		});
	result.register_decoder(meter_two_hour_open_unbalance_format,
		[](const MeterRecord &record, SystemTime received_at) {
			return decode_two_hour_open_unbalance_meter_record(
				record, received_at);
		});
	result.register_decoder(meter_periodic_format,
		[](const MeterRecord &record, SystemTime received_at) {
			return decode_periodic_meter_record(record,
							    received_at);
		});
	/* 150/180-cycle aggregates interleave with basic records on the same DMA
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
	result.register_decoder(meter_ten_minute_open_format,
		[](const MeterRecord &record, SystemTime received_at) {
			return decode_ten_minute_open_meter_record(record,
							    received_at);
		});
	result.register_decoder(meter_two_hour_open_format,
		[](const MeterRecord &record, SystemTime received_at) {
			return decode_two_hour_open_meter_record(record,
							 received_at);
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

PowerQualityEventLifecycleSnapshot
decode_pq_event_lifecycle_record(const MeterRecord &record)
{
	if (!record.header_valid() ||
	    record.record_format() != meter_pq_event_lifecycle_format)
		throw std::invalid_argument("invalid PQ-EVENT-v1 record header");
	if (record.sequence() == 0u || record.configuration_generation() == 0u ||
	    record.sample_rate_hz() == 0u || record.emit_drops() != 0u)
		throw std::invalid_argument("invalid PQ-EVENT-v1 common provenance");
	if ((record.status() & ~0x0fu) != 0u ||
	    (record.status() & 0x0au) != 0x0au)
		throw std::invalid_argument("invalid PQ-EVENT-v1 status");

	const auto identity = record.word(13);
	if ((identity & ~0x000307f3u) != 0u)
		throw std::invalid_argument("PQ-EVENT-v1 identity has reserved bits");
	const auto lifecycle = static_cast<std::uint8_t>(identity & 0x3u);
	const auto type = static_cast<std::uint8_t>((identity >> 4u) & 0x0fu);
	const auto phase_mask = static_cast<std::uint8_t>((identity >> 8u) & 0x7u);
	const auto trigger_source = static_cast<std::uint8_t>(
		(identity >> 16u) & 0x3u);
	if (lifecycle > meter_event_lifecycle_abort || type > 8u ||
	    phase_mask == 0u || trigger_source > 2u)
		throw std::invalid_argument("PQ-EVENT-v1 identity is out of range");

	PowerQualityEventLifecycleSnapshot result{};
	result.lifecycle = static_cast<PowerQualityEventLifecycle>(lifecycle);
	result.type = static_cast<PowerQualityLifecycleType>(type);
	result.phase_mask = phase_mask;
	result.trigger_source = trigger_source;
	result.sequence = record.sequence();
	result.configuration_generation = record.configuration_generation();
	result.profile_generation = record.word(meter_event_profile_generation_word);
	result.sample_rate_hz = record.sample_rate_hz();
	result.first_sample = record.first_sample_index();
	result.last_sample = record.unsigned64(meter_event_last_sample_word);
	result.id = {record.unsigned64(meter_event_id_word),
		record.unsigned64(meter_event_id_word + 2u)};
	result.threshold_e4 = record.word(meter_event_threshold_word);
	result.hysteresis_e4 = record.word(meter_event_hysteresis_word);
	const auto waveform = record.word(meter_event_waveform_policy_word);
	result.waveform_enabled = (waveform & 0x1u) != 0u;
	result.per_phase = (waveform & 0x2u) != 0u;
	result.iec_classification = (waveform & 0x4u) != 0u;
	result.waveform_decimation = waveform >> 8u;
	result.waveform_pretrigger_ms =
		record.word(meter_event_waveform_pre_ms_word);
	result.waveform_posttrigger_ms =
		record.word(meter_event_waveform_post_ms_word);
	result.reference_micro_units = record.word(meter_event_reference_word);
	for (std::size_t phase = 0; phase < 3u; ++phase) {
		result.minimum_micro_units[phase] =
			record.word(meter_event_minimum_word + phase);
		result.maximum_micro_units[phase] =
			record.word(meter_event_maximum_word + phase);
		result.current_micro_units[phase] =
			record.word(meter_event_current_word + phase);
	}
	result.duration_samples = record.unsigned64(meter_event_duration_word);
	result.trigger_sample = record.unsigned64(meter_event_trigger_sample_word);
	result.start_utc_nanoseconds =
		record.unsigned64(meter_event_start_utc_ns_word);
	result.last_utc_nanoseconds =
		record.unsigned64(meter_event_last_utc_ns_word);
	result.time_quality = static_cast<TimeQuality>(
		record.word(meter_event_time_quality_word));
	result.discontinuities = record.word(meter_event_discontinuity_word);
	result.update_count = record.word(47u);
	for (std::size_t lane = 0; lane < result.settings_digest.size(); ++lane)
		result.settings_digest[lane] =
			record.word(meter_event_settings_digest_word + lane);
	result.valid_mask = record.word(7u);
	result.status = record.status();

	const auto valid_decimation = [](std::uint32_t value) {
		return value == 1u || value == 2u || value == 4u || value == 8u ||
		       value == 16u || value == 32u;
	};
	if (result.id.session == 0u || result.id.counter == 0u ||
	    result.profile_generation != result.configuration_generation ||
	    result.last_sample < result.first_sample ||
	    result.trigger_sample < result.first_sample ||
	    result.trigger_sample > result.last_sample ||
	    result.duration_samples != result.last_sample - result.first_sample ||
	    result.update_count == 0u || result.reference_micro_units == 0u ||
	    result.threshold_e4 == 0u || result.threshold_e4 > 0xffffu ||
	    result.hysteresis_e4 >= result.threshold_e4 ||
	    result.waveform_pretrigger_ms > 120000u ||
	    result.waveform_posttrigger_ms > 120000u ||
	    !valid_decimation(result.waveform_decimation))
		throw std::invalid_argument("PQ-EVENT-v1 provenance is inconsistent");
	if ((waveform & ~0x00003f07u) != 0u)
		throw std::invalid_argument("PQ-EVENT-v1 waveform policy has reserved bits");
	const bool expected_iec = type <= 3u || type == 8u;
	if (result.iec_classification != expected_iec)
		throw std::invalid_argument("PQ-EVENT-v1 taxonomy is inconsistent");
	const auto expected_valid = result.voltage_event()
		? static_cast<std::uint32_t>(phase_mask) << 4u
		: static_cast<std::uint32_t>(phase_mask);
	if (result.valid_mask != expected_valid)
		throw std::invalid_argument("PQ-EVENT-v1 phase validity is inconsistent");
	const auto covered = result.duration_samples ==
		std::numeric_limits<std::uint64_t>::max()
		? std::numeric_limits<std::uint64_t>::max()
		: result.duration_samples + 1u;
	const auto expected_count = static_cast<std::uint32_t>(std::min(
		covered, static_cast<std::uint64_t>(
			std::numeric_limits<std::uint32_t>::max())));
	if (record.block_sample_count() != expected_count ||
	    result.discontinuities != record.result_drops())
		throw std::invalid_argument("PQ-EVENT-v1 span is inconsistent");
	for (std::size_t phase = 0; phase < 3u; ++phase)
		if (result.minimum_micro_units[phase] >
				result.maximum_micro_units[phase] ||
		    result.current_micro_units[phase] <
				result.minimum_micro_units[phase] ||
		    result.current_micro_units[phase] >
				result.maximum_micro_units[phase])
			throw std::invalid_argument(
				"PQ-EVENT-v1 extrema are inconsistent");
	const auto quality = static_cast<std::uint32_t>(result.time_quality);
	if (quality > static_cast<std::uint32_t>(TimeQuality::Holdover) ||
	    (quality == 0u && (result.start_utc_nanoseconds != 0u ||
			       result.last_utc_nanoseconds != 0u)) ||
	    (quality != 0u && (result.start_utc_nanoseconds == 0u ||
			       result.last_utc_nanoseconds <
				       result.start_utc_nanoseconds)))
		throw std::invalid_argument("PQ-EVENT-v1 time provenance is inconsistent");
	bool digest_present = false;
	for (const auto word : result.settings_digest)
		digest_present = digest_present || word != 0u;
	if (!digest_present || record.word(27u) != 0u)
		throw std::invalid_argument("PQ-EVENT-v1 reserved/snapshot fields are invalid");
	for (std::size_t word = 52u; word < meter_record_word_count; ++word)
		if (record.word(word) != 0u)
			throw std::invalid_argument("PQ-EVENT-v1 reserved word is nonzero");
	return result;
}

FlickerSnapshot decode_flicker_record(const MeterRecord &record)
{
	if (!record.header_valid() ||
	    record.record_format() != meter_flicker_format)
		throw std::invalid_argument("invalid FLICKER-v1 record header");
	if (record.sequence() == 0u || record.configuration_generation() == 0u ||
	    record.sample_rate_hz() < 2000u ||
	    record.sample_rate_hz() > 128000u ||
	    record.sample_rate_hz() % 2000u != 0u ||
	    record.emit_drops() != 0u || record.result_drops() != 0u)
		throw std::invalid_argument("invalid FLICKER-v1 common provenance");
	if ((record.status() & ~0x5u) != 0u)
		throw std::invalid_argument("invalid FLICKER-v1 status");

	const auto identity = record.word(13u);
	const auto kind = static_cast<std::uint8_t>(identity & 0xffu);
	const auto phase_mask = static_cast<std::uint8_t>((identity >> 8u) & 0x7u);
	if ((identity & ~0x000007ffu) != 0u || kind > meter_flicker_kind_plt ||
	    record.valid_mask() != static_cast<std::uint8_t>(phase_mask << 4u))
		throw std::invalid_argument("invalid FLICKER-v1 kind or phase mask");

	FlickerSnapshot result{};
	result.kind = static_cast<FlickerRecordKind>(kind);
	result.sequence = record.sequence();
	result.configuration_generation = record.configuration_generation();
	result.profile_generation =
		record.word(meter_flicker_profile_generation_word);
	result.sample_rate_hz = record.sample_rate_hz();
	result.first_sample = record.first_sample_index();
	result.last_sample = record.unsigned64(meter_flicker_last_sample_word);
	result.sample_count = record.block_sample_count();
	result.interval_seconds =
		record.word(meter_flicker_interval_seconds_word);
	result.phase_valid_mask = phase_mask;
	const auto model = record.word(meter_flicker_model_word);
	result.lamp_voltage = static_cast<std::uint16_t>(model & 0xffffu);
	result.nominal_frequency_hz = static_cast<std::uint8_t>(model >> 16u);
	result.status = record.status();
	result.source_status = record.word(meter_flicker_source_status_word);
	for (std::size_t phase = 0u; phase < 3u; ++phase) {
		result.pinst_q16[phase] =
			record.word(meter_flicker_pinst_word + phase);
		result.pst_q16[phase] = record.word(meter_flicker_pst_word + phase);
		result.plt_q16[phase] = record.word(meter_flicker_plt_word + phase);
		result.valid_internal_samples[phase] =
			record.word(meter_flicker_valid_count_word + phase);
	}

	const std::uint32_t expected_seconds =
		kind == meter_flicker_kind_live ? 1u :
		kind == meter_flicker_kind_pst ? 600u : 7200u;
	const std::uint32_t expected_internal = expected_seconds * 2000u;
	const auto expected_span = static_cast<std::uint64_t>(expected_seconds) *
		result.sample_rate_hz;
	std::uint32_t maximum_valid = 0u;
	for (std::size_t phase = 0u; phase < 3u; ++phase) {
		const auto count = result.valid_internal_samples[phase];
		maximum_valid = std::max(maximum_valid, count);
		if (count > expected_internal ||
		    ((phase_mask & (1u << phase)) != 0u &&
		     count != expected_internal))
			throw std::invalid_argument(
				"FLICKER-v1 phase validity is inconsistent");
	}
	const auto expected_count = static_cast<std::uint64_t>(maximum_valid) *
		(result.sample_rate_hz / 2000u);
	if (result.profile_generation != result.configuration_generation ||
	    result.interval_seconds != expected_seconds ||
	    result.last_sample < result.first_sample ||
	    result.last_sample - result.first_sample + 1u != expected_span ||
	    result.sample_count != expected_count ||
	    record.unsigned64(meter_flicker_interval_first_word) !=
		    result.first_sample ||
	    (model & 0xff000000u) != 0u ||
	    (result.lamp_voltage != 120u && result.lamp_voltage != 230u) ||
	    (result.nominal_frequency_hz != 50u &&
	     result.nominal_frequency_hz != 60u) ||
	    (result.source_status & ~0xffu) != 0u)
		throw std::invalid_argument("FLICKER-v1 provenance is inconsistent");
	if (kind != meter_flicker_kind_live &&
	    (result.source_status & (1u << 7u)) != 0u)
		throw std::invalid_argument("completed FLICKER-v1 interval is settling");
	if (kind == meter_flicker_kind_live) {
		for (std::size_t phase = 0u; phase < 3u; ++phase)
			if (result.pst_q16[phase] != 0u || result.plt_q16[phase] != 0u)
				throw std::invalid_argument(
					"live FLICKER-v1 carries completed metrics");
	} else if (kind == meter_flicker_kind_pst) {
		for (const auto value : result.plt_q16)
			if (value != 0u)
				throw std::invalid_argument(
					"Pst FLICKER-v1 carries a Plt result");
	} else if (phase_mask == 0u) {
		throw std::invalid_argument("Plt FLICKER-v1 has no valid phase");
	}
	for (std::size_t word = 34u; word < meter_record_word_count; ++word)
		if (record.word(word) != 0u)
			throw std::invalid_argument("FLICKER-v1 reserved word is nonzero");
	return result;
}

MainsSignalSnapshot decode_mains_signal_record(const MeterRecord &record)
{
	if (!record.header_valid() ||
	    record.record_format() != meter_mains_signal_format)
		throw std::invalid_argument("invalid MAINS-SIGNAL-v1 record header");
	const auto rate = record.sample_rate_hz();
	const bool supported_rate = rate == 2000u || rate == 4000u ||
		rate == 8000u || rate == 16000u || rate == 32000u ||
		rate == 64000u || rate == 128000u;
	if (record.sequence() == 0u || record.configuration_generation() == 0u ||
	    !supported_rate || record.emit_drops() != 0u ||
	    record.result_drops() != 0u)
		throw std::invalid_argument(
			"invalid MAINS-SIGNAL-v1 common provenance");
	if ((record.status() & ~0x5u) != 0u)
		throw std::invalid_argument("invalid MAINS-SIGNAL-v1 status");

	const auto identity = record.word(13u);
	const auto valid_mask = static_cast<std::uint8_t>(identity & 0x7u);
	const auto detected_mask =
		static_cast<std::uint8_t>((identity >> 8u) & 0x7u);
	if ((identity & ~0x00000707u) != 0u ||
	    (detected_mask & ~valid_mask) != 0u ||
	    record.word(7u) != static_cast<std::uint32_t>(valid_mask) << 4u)
		throw std::invalid_argument(
			"invalid MAINS-SIGNAL-v1 phase identity");

	MainsSignalSnapshot result{};
	result.sequence = record.sequence();
	result.configuration_generation = record.configuration_generation();
	result.profile_generation =
		record.word(meter_mains_profile_generation_word);
	result.sample_rate_hz = rate;
	result.first_sample = record.first_sample_index();
	result.last_sample = record.unsigned64(meter_mains_last_sample_word);
	result.sample_count = record.block_sample_count();
	result.phase_valid_mask = valid_mask;
	result.detected_phase_mask = detected_mask;
	result.configured_millihz =
		record.word(meter_mains_configured_millihz_word);
	result.measured_millihz =
		record.word(meter_mains_measured_millihz_word);
	result.bandwidth_millihz =
		record.word(meter_mains_bandwidth_millihz_word);
	result.observation_ms = record.word(meter_mains_observation_ms_word);
	result.source_status = record.word(meter_mains_source_status_word);
	result.threshold_e4 = record.word(meter_mains_threshold_e4_word);
	result.reference_microvolts =
		record.word(meter_mains_reference_microvolts_word);
	result.status = record.status();
	for (std::size_t phase = 0u; phase < 3u; ++phase) {
		result.magnitude_microvolts[phase] =
			record.word(meter_mains_magnitude_word + phase);
		result.background_microvolts[phase] =
			record.word(meter_mains_background_word + phase);
	}

	const auto upper_frequency =
		static_cast<std::uint64_t>(result.configured_millihz) +
		result.bandwidth_millihz;
	const auto nyquist_millihz = static_cast<std::uint64_t>(rate) * 500u;
	const auto expected_samples = rate / 5u;
	if (result.profile_generation != result.configuration_generation ||
	    result.configured_millihz == 0u ||
	    result.bandwidth_millihz < 4u ||
	    result.bandwidth_millihz >= result.configured_millihz ||
	    upper_frequency >= nyquist_millihz ||
	    upper_frequency >= 12500000u || result.observation_ms != 200u ||
	    result.threshold_e4 > 0xffffu || result.reference_microvolts == 0u ||
	    result.sample_count != expected_samples ||
	    result.last_sample < result.first_sample ||
	    result.last_sample - result.first_sample + 1u != expected_samples ||
	    (result.source_status & ~0x3fu) != 0u ||
	    (result.source_status & 1u) == 0u)
		throw std::invalid_argument(
			"MAINS-SIGNAL-v1 provenance is inconsistent");

	const bool source_arithmetic =
		(result.source_status & (1u << 4u)) != 0u;
	const bool public_arithmetic = (result.status & 1u) != 0u;
	if (source_arithmetic != public_arithmetic ||
	    ((result.source_status & (1u << 3u)) != 0u &&
	     (result.status & (1u << 2u)) == 0u))
		throw std::invalid_argument(
			"MAINS-SIGNAL-v1 status provenance is inconsistent");

	const auto threshold_microvolts =
		(static_cast<std::uint64_t>(result.reference_microvolts) *
			 result.threshold_e4 + 9999u) /
		10000u;
	std::uint8_t expected_detected = 0u;
	bool background_dominant = false;
	for (std::size_t phase = 0u; phase < 3u; ++phase) {
		if ((valid_mask & (1u << phase)) == 0u)
			continue;
		if (result.magnitude_microvolts[phase] >= threshold_microvolts)
			expected_detected |= static_cast<std::uint8_t>(1u << phase);
		if (result.background_microvolts[phase] >
		    result.magnitude_microvolts[phase])
			background_dominant = true;
	}
	if (detected_mask != expected_detected ||
	    background_dominant !=
		((result.source_status & (1u << 5u)) != 0u))
		throw std::invalid_argument(
			"MAINS-SIGNAL-v1 detection result is inconsistent");

	if (detected_mask == 0u) {
		if (result.measured_millihz != result.configured_millihz)
			throw std::invalid_argument(
				"MAINS-SIGNAL-v1 idle frequency is inconsistent");
	} else {
		const auto half_bandwidth = result.bandwidth_millihz / 2u;
		const auto lower = result.configured_millihz - half_bandwidth;
		const auto upper = static_cast<std::uint64_t>(
			result.configured_millihz) + half_bandwidth;
		if (result.measured_millihz < lower ||
		    result.measured_millihz > upper)
			throw std::invalid_argument(
				"MAINS-SIGNAL-v1 measured frequency is out of band");
	}

	for (std::size_t word = 30u; word < meter_record_word_count; ++word)
		if (record.word(word) != 0u)
			throw std::invalid_argument(
				"MAINS-SIGNAL-v1 reserved word is nonzero");
	return result;
}

} // namespace msap1
