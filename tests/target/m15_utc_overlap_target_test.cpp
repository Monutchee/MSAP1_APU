#include "msap1/meter/MeterDataProvider/stream/meter_stream_ipc.hpp"
#include "msap1/meter/meter_record.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <unistd.h>

namespace {

class ConsumerGuard {
public:
	ConsumerGuard(msap1::meter_stream::MeterRecordStreamClient &client,
		std::string name) : client_(client), name_(std::move(name))
	{
		try {
			client_.unregister_consumer(name_);
		} catch (...) {
		}
		client_.register_consumer(name_);
	}

	~ConsumerGuard()
	{
		try {
			client_.unregister_consumer(name_);
		} catch (...) {
		}
	}

	const std::string &name() const { return name_; }

private:
	msap1::meter_stream::MeterRecordStreamClient &client_;
	std::string name_;
};

struct BasicView {
	std::uint32_t sequence{};
	std::uint32_t sample_count{};
	std::uint64_t first_sample{};
	std::uint64_t last_sample{};
	std::uint32_t timing_word{};
	std::uint8_t nominal_hz{};
	std::uint8_t cycle_count{};
};

struct AggregateView {
	std::uint32_t sequence{};
	std::uint32_t sample_count{};
	std::uint64_t first_sample{};
	std::uint64_t last_sample{};
	std::uint32_t status_word{};
	std::uint32_t first_basic{};
	std::uint32_t last_basic{};
	std::uint8_t basic_count{};
	std::uint8_t nominal_hz{};
	std::uint16_t cycle_count{};
	std::uint32_t reset_count{};
	std::uint32_t ineligible_count{};
	std::uint32_t continuity_count{};
};

void require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

std::uint64_t inclusive_span(std::uint64_t first, std::uint64_t last)
{
	require(last >= first, "sample range runs backwards");
	return last - first + 1u;
}

std::uint32_t inclusive_sequence_count(std::uint32_t first,
	std::uint32_t last)
{
	return last - first + 1u;
}

bool sequence_range_contains(std::uint32_t first, std::uint32_t last,
	std::uint32_t value)
{
	return value - first <= last - first;
}

BasicView basic_view(const msap1::MeterRecord &record)
{
	const auto timing = record.timing();
	return {
		record.sequence(),
		record.block_sample_count(),
		record.first_sample_index(),
		static_cast<std::uint64_t>(record.word(14)) |
			(static_cast<std::uint64_t>(record.word(15)) << 32u),
		record.word(13),
		timing.nominal_frequency_hz,
		timing.cycle_count,
	};
}

AggregateView aggregate_view(const msap1::MeterRecord &record)
{
	const auto shape = record.aggregate_composition();
	return {
		record.aggregate_sequence(),
		record.aggregate_sample_count(),
		record.aggregate_first_sample_index(),
		record.aggregate_last_sample_index(),
		record.word(8),
		record.first_basic_sequence(),
		record.last_basic_sequence(),
		shape.basic_block_count,
		shape.nominal_frequency_hz,
		shape.cycle_count,
		record.aggregate_reset_count(),
		record.aggregate_ineligible_count(),
		record.aggregate_continuity_count(),
	};
}

void print_basic(const char *label, const BasicView &basic)
{
	std::cout << label << ",seq=" << basic.sequence
		<< ",count=" << basic.sample_count
		<< ",first=" << basic.first_sample
		<< ",last=" << basic.last_sample
		<< ",span=" << inclusive_span(
			basic.first_sample, basic.last_sample)
		<< ",timing=0x" << std::hex << basic.timing_word << std::dec
		<< ",nominal_hz=" << unsigned(basic.nominal_hz)
		<< ",cycles=" << unsigned(basic.cycle_count) << '\n';
}

void print_aggregate(const char *label, const AggregateView &aggregate)
{
	std::cout << label << ",seq=" << aggregate.sequence
		<< ",count=" << aggregate.sample_count
		<< ",first=" << aggregate.first_sample
		<< ",last=" << aggregate.last_sample
		<< ",span=" << inclusive_span(
			aggregate.first_sample, aggregate.last_sample)
		<< ",first_basic=" << aggregate.first_basic
		<< ",last_basic=" << aggregate.last_basic
		<< ",blocks=" << unsigned(aggregate.basic_count)
		<< ",cycles=" << aggregate.cycle_count
		<< ",status=0x" << std::hex << aggregate.status_word << std::dec
		<< ",reset=" << aggregate.reset_count
		<< ",ineligible=" << aggregate.ineligible_count
		<< ",continuity=" << aggregate.continuity_count << '\n';
}

void validate(const BasicView &previous, const BasicView &synchronized,
	const AggregateView &continuing, const AggregateView &new_cadence)
{
	require(previous.nominal_hz == synchronized.nominal_hz,
		"Basic nominal frequency changed at the UTC boundary");
	require(previous.cycle_count == synchronized.cycle_count,
		"Basic cycle count changed at the UTC boundary");
	require(inclusive_span(previous.first_sample, previous.last_sample) ==
		previous.sample_count, "pre-boundary Basic sample range is inconsistent");
	require(inclusive_span(synchronized.first_sample,
		synchronized.last_sample) == synchronized.sample_count,
		"synchronized Basic sample range is inconsistent");
	require(synchronized.first_sample <= previous.last_sample,
		"synchronized Basic does not overlap its predecessor");
	require(synchronized.last_sample > previous.last_sample,
		"synchronized Basic is a contained duplicate");

	const auto basic_overlap = previous.last_sample -
		synchronized.first_sample + 1u;
	const auto expected_aggregate_cycles =
		static_cast<std::uint16_t>(15u * synchronized.cycle_count);
	for (const auto *aggregate : {&continuing, &new_cadence}) {
		require(aggregate->basic_count == 15u,
			"aggregate does not contain exactly 15 Basic blocks");
		require(inclusive_sequence_count(aggregate->first_basic,
			aggregate->last_basic) == 15u,
			"aggregate Basic sequence range is not consecutive");
		require(aggregate->nominal_hz == synchronized.nominal_hz,
			"aggregate nominal frequency does not match Basic");
		require(aggregate->cycle_count == expected_aggregate_cycles,
			"aggregate cycle count is not 150/180");
		require(aggregate->continuity_count == 0u,
			"aggregate reports a continuity error");
	}

	require(sequence_range_contains(continuing.first_basic,
		continuing.last_basic, synchronized.sequence),
		"continuing aggregate does not contain the synchronized Basic");
	require(new_cadence.first_basic == synchronized.sequence,
		"new-cadence aggregate does not start at the synchronized Basic");
	require(new_cadence.sequence == continuing.sequence + 1u,
		"continuing and new-cadence aggregates are not consecutive");
	require(new_cadence.first_sample == synchronized.first_sample,
		"new-cadence aggregate does not start at the synchronized sample");

	const auto continuing_span = inclusive_span(
		continuing.first_sample, continuing.last_sample);
	require(continuing.sample_count >= continuing_span,
		"continuing aggregate contribution count is shorter than its span");
	require(continuing.sample_count - continuing_span == basic_overlap,
		"continuing aggregate overlap does not match the Basic overlap");
	require(new_cadence.sample_count == inclusive_span(
		new_cadence.first_sample, new_cadence.last_sample),
		"new-cadence aggregate duplicates or omits physical samples");
}

} // namespace

int main(int argc, char **argv)
try {
	if (argc != 2)
		throw std::invalid_argument(
			"usage: m15_utc_overlap_target_test SECONDS");
	const auto duration_value = std::stoul(argv[1]);
	if (duration_value == 0u)
		throw std::invalid_argument("SECONDS must be greater than zero");
	const auto duration = std::chrono::seconds(duration_value);

	msap1::meter_stream::MeterRecordStreamClient client;
	ConsumerGuard consumer(client,
		"m15-utc-overlap-" + std::to_string(::getpid()));

	/* Observe only records produced during this capture window. A new durable
	 * consumer starts at cursor zero; replaying the retained spool can monopolize
	 * meter-stream and perturb the DMA path being tested. */
	const auto stream_head = client.status().newest_cursor;
	if (stream_head != 0u)
		client.acknowledge(consumer.name(), stream_head);
	std::cout << "START,stream_cursor=" << stream_head << '\n' << std::flush;

	std::optional<BasicView> previous_basic;
	std::optional<std::pair<BasicView, BasicView>> boundary_basics;
	std::optional<AggregateView> continuing_aggregate;
	std::optional<AggregateView> synchronized_aggregate;
	std::uint64_t records = 0;
	const auto deadline = std::chrono::steady_clock::now() + duration;
	while (std::chrono::steady_clock::now() < deadline) {
		auto batch = client.read_after(consumer.name(), 4096);
		if (batch.empty()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			continue;
		}

		for (const auto &envelope : batch) {
			++records;
			if (envelope.payload.size() != sizeof(msap1::MeterRecord))
				continue;
			msap1::MeterRecord record;
			std::memcpy(&record, envelope.payload.data(), sizeof(record));
			require(record.header_valid(), "captured record header is invalid");

			if (envelope.record_format == msap1::meter_periodic_format) {
				const auto current = basic_view(record);
				if (record.timing().utc_resynchronized &&
					!boundary_basics) {
					require(previous_basic.has_value(),
						"capture started too late to observe the preceding Basic");
					boundary_basics.emplace(*previous_basic, current);
					/* Discard a synchronized aggregate that might have closed just
					 * after this capture began but belonged to the prior boundary. */
					continuing_aggregate.reset();
					synchronized_aggregate.reset();
					print_basic("BASIC_PREVIOUS", *previous_basic);
					print_basic("BASIC_SYNCHRONIZED", current);
				}
				previous_basic = current;
			} else if (envelope.record_format ==
				msap1::meter_aggregate_format) {
				const auto status = record.aggregate_status();
				if (status.utc_overlap && !continuing_aggregate) {
					require(status.complete && !status.utc_resynchronized,
						"continuing aggregate status flags are inconsistent");
					continuing_aggregate = aggregate_view(record);
					print_aggregate("AGG_CONTINUING", *continuing_aggregate);
				} else if (status.utc_resynchronized &&
					!synchronized_aggregate) {
					require(status.complete && !status.utc_overlap,
						"new-cadence aggregate status flags are inconsistent");
					synchronized_aggregate = aggregate_view(record);
					print_aggregate("AGG_SYNCHRONIZED",
						*synchronized_aggregate);
				}
			}
		}
		client.acknowledge(consumer.name(), batch.back().cursor);
	}

	require(boundary_basics.has_value(),
		"no UTC-resynchronized Basic pair was captured");
	require(continuing_aggregate.has_value(),
		"no continuing UTC-overlap aggregate was captured");
	require(synchronized_aggregate.has_value(),
		"no synchronized 150/180-cycle aggregate was captured");
	validate(boundary_basics->first, boundary_basics->second,
		*continuing_aggregate, *synchronized_aggregate);
	std::cout << "PASS,records=" << records
		<< ",nominal_hz=" << unsigned(boundary_basics->second.nominal_hz)
		<< ",basic_overlap="
		<< boundary_basics->first.last_sample -
			boundary_basics->second.first_sample + 1u << '\n';
	return 0;
} catch (const std::exception &error) {
	std::cerr << "FAIL," << error.what() << '\n';
	return 1;
}
