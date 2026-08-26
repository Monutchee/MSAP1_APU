#include "msap1/meter/harmonic_spectrum.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace msap1 {
namespace {

constexpr std::uint64_t magnitude_mask = (std::uint64_t{1} << 40) - 1;
constexpr std::uint64_t angle_mask = (std::uint64_t{1} << 20) - 1;
constexpr std::uint32_t harmonic_known_status = 0xffu;

std::uint8_t expected_order_count(std::uint8_t chunk)
{
	const auto first = static_cast<std::size_t>(chunk) *
			 harmonic_orders_per_record + 1;
	return static_cast<std::uint8_t>(std::min(
		harmonic_orders_per_record, harmonic_max_order - first + 1));
}

HarmonicSpectrumSnapshot snapshot_from(const HarmonicRecordChunk &chunk)
{
	HarmonicSpectrumSnapshot result{};
	result.sequence = chunk.sequence;
	result.configuration_generation = chunk.configuration_generation;
	result.sample_rate_hz = chunk.sample_rate_hz;
	result.sample_count = chunk.sample_count;
	result.valid_mask = chunk.valid_mask;
	result.status = chunk.status;
	result.first_sample = chunk.first_sample;
	result.emit_drops = chunk.emit_drops;
	result.result_drops = chunk.result_drops;
	result.measured_frequency_millihz =
		chunk.measured_frequency_millihz;
	result.qualified_max_order = chunk.qualified_max_order;
	result.nominal_frequency_hz = chunk.nominal_frequency_hz;
	result.cycle_count = chunk.cycle_count;
	result.filter_profile_id = chunk.filter_profile_id;
	for (std::size_t channel = 0; channel < result.channels.size(); ++channel)
		for (std::size_t index = 0; index < result.channels[channel].size();
		     ++index)
			result.channels[channel][index].order =
				static_cast<std::uint8_t>(index + 1);
	return result;
}

bool same_family(const HarmonicSpectrumSnapshot &family,
		 const HarmonicRecordChunk &chunk)
{
	return family.sequence == chunk.sequence &&
	       family.configuration_generation == chunk.configuration_generation &&
	       family.sample_rate_hz == chunk.sample_rate_hz &&
	       family.sample_count == chunk.sample_count &&
	       family.valid_mask == chunk.valid_mask &&
	       family.status == chunk.status &&
	       family.first_sample == chunk.first_sample &&
	       family.emit_drops == chunk.emit_drops &&
	       family.result_drops == chunk.result_drops &&
	       family.measured_frequency_millihz ==
		       chunk.measured_frequency_millihz &&
	       family.qualified_max_order == chunk.qualified_max_order &&
	       family.nominal_frequency_hz == chunk.nominal_frequency_hz &&
	       family.cycle_count == chunk.cycle_count &&
	       family.filter_profile_id == chunk.filter_profile_id;
}

bool forward_of(std::uint32_t candidate, std::uint32_t baseline)
{
	const auto distance = candidate - baseline;
	return distance != 0u && distance < (std::uint32_t{1} << 31u);
}

} // namespace

HarmonicRecordChunk decode_harmonic_record(const MeterRecord &record)
{
	if (!record.header_valid() ||
	    record.record_format() != meter_harmonic_format)
		throw std::invalid_argument("invalid harmonic record header");
	if (record.sample_rate_hz() == 0u || record.block_sample_count() == 0u)
		throw std::invalid_argument("harmonic record has an empty sample span");
	if ((record.word(7) & ~0x7fu) != 0u)
		throw std::invalid_argument("harmonic record has an invalid lane mask");
	if ((record.status() & ~harmonic_known_status) != 0u ||
	    (record.status() & 0x2u) == 0u)
		throw std::invalid_argument("harmonic record status is malformed");

	HarmonicRecordChunk result{};
	result.sequence = record.sequence();
	result.configuration_generation = record.configuration_generation();
	result.sample_rate_hz = record.sample_rate_hz();
	result.sample_count = record.block_sample_count();
	result.valid_mask = record.valid_mask();
	result.status = record.status();
	result.first_sample = record.first_sample_index();
	result.emit_drops = record.emit_drops();
	result.result_drops = record.result_drops();

	const auto header = record.word(13);
	result.channel = static_cast<std::uint8_t>(header & 0x7u);
	result.chunk = static_cast<std::uint8_t>((header >> 3) & 0xfu);
	result.first_order = static_cast<std::uint8_t>((header >> 7) & 0xffu);
	result.order_count = static_cast<std::uint8_t>((header >> 15) & 0x1fu);
	result.chunk_count = static_cast<std::uint8_t>((header >> 20) & 0xfu);
	result.max_order = static_cast<std::uint8_t>(header >> 24);
	if (result.channel >= harmonic_channel_count ||
	    result.chunk >= harmonic_chunks_per_channel ||
	    result.chunk_count != harmonic_chunks_per_channel ||
	    result.max_order != harmonic_max_order ||
	    result.first_order !=
		    result.chunk * harmonic_orders_per_record + 1 ||
	    result.order_count != expected_order_count(result.chunk))
		throw std::invalid_argument("harmonic record chunk geometry is malformed");

	result.measured_frequency_millihz = record.word(14);
	const auto metadata = record.word(15);
	result.qualified_max_order = static_cast<std::uint8_t>(metadata);
	result.nominal_frequency_hz = static_cast<std::uint8_t>(metadata >> 8);
	result.cycle_count = static_cast<std::uint8_t>(metadata >> 16);
	result.filter_profile_id = static_cast<std::uint8_t>(metadata >> 24);
	if (result.qualified_max_order > harmonic_max_order ||
	    !((result.nominal_frequency_hz == 50u && result.cycle_count == 10u) ||
	      (result.nominal_frequency_hz == 60u && result.cycle_count == 12u)))
		throw std::invalid_argument("harmonic record metadata is malformed");
	if (((result.status & 0x20u) != 0u) !=
	    (result.qualified_max_order == harmonic_max_order))
		throw std::invalid_argument("harmonic full-range status disagrees with metadata");
	if (result.qualified_max_order < harmonic_max_order &&
	    (result.status & 0x80u) == 0u)
		throw std::invalid_argument("limited harmonic range lacks rate-limit status");
	if ((result.status & 0x4u) != 0u &&
	    result.measured_frequency_millihz == 0u)
		throw std::invalid_argument("grid-locked harmonic record has zero frequency");

	for (std::size_t index = 0; index < harmonic_orders_per_record; ++index) {
		const auto packed = record.unsigned64(16 + index * 2);
		if (index >= result.order_count) {
			if (packed != 0u)
				throw std::invalid_argument(
					"harmonic record has nonzero padding entries");
			continue;
		}
		if ((packed >> 62) != 0u)
			throw std::invalid_argument("harmonic entry uses reserved bits");
		auto &entry = result.entries[index];
		entry.order = static_cast<std::uint8_t>(result.first_order + index);
		entry.magnitude_micro_units = packed & magnitude_mask;
		entry.angle_millidegrees = static_cast<std::uint32_t>(
			(packed >> 40) & angle_mask);
		entry.magnitude_valid = (packed & (std::uint64_t{1} << 60)) != 0;
		entry.angle_valid = (packed & (std::uint64_t{1} << 61)) != 0;
		if ((!entry.magnitude_valid && entry.magnitude_micro_units != 0u) ||
		    (!entry.angle_valid && entry.angle_millidegrees != 0u) ||
		    (entry.angle_valid &&
		     (!entry.magnitude_valid || entry.angle_millidegrees >= 360000u)) ||
		    (entry.order > result.qualified_max_order &&
		     (entry.magnitude_valid || entry.angle_valid)) ||
		    ((result.valid_mask & (1u << result.channel)) == 0u &&
		     (entry.magnitude_valid || entry.angle_valid)))
			throw std::invalid_argument("harmonic entry validity is malformed");
	}
	return result;
}

HarmonicAssemblyUpdate HarmonicFamilyAssembler::accept(
	const HarmonicRecordChunk &chunk)
{
	HarmonicAssemblyUpdate update{};
	bool replaced_partial = false;
	if (partial_ && chunk.sequence != partial_->snapshot.sequence) {
		if (!forward_of(chunk.sequence, partial_->snapshot.sequence))
			throw std::invalid_argument("stale harmonic family sequence");
		update.incomplete_families =
			chunk.sequence - partial_->snapshot.sequence;
		partial_.reset();
		replaced_partial = true;
	}
	if (!partial_) {
		/* A replacement is measured from the partial family because any
		 * earlier gap was already reported when that partial began. */
		if (last_completed_sequence_ && !replaced_partial) {
			if (!forward_of(chunk.sequence, *last_completed_sequence_))
				throw std::invalid_argument("stale harmonic family sequence");
			const auto expected = *last_completed_sequence_ + 1u;
			update.incomplete_families += chunk.sequence - expected;
		}
		partial_.emplace();
		partial_->snapshot = snapshot_from(chunk);
	}
	if (!same_family(partial_->snapshot, chunk))
		throw std::invalid_argument("harmonic family provenance mismatch");

	const auto record_index =
		static_cast<std::size_t>(chunk.channel) *
			harmonic_chunks_per_channel + chunk.chunk;
	if (partial_->received[record_index])
		throw std::invalid_argument("duplicate harmonic family chunk");
	partial_->received[record_index] = true;
	++partial_->received_count;
	for (std::size_t index = 0; index < chunk.order_count; ++index)
		partial_->snapshot.channels[chunk.channel]
			[chunk.first_order + index - 1] = chunk.entries[index];

	if (partial_->received_count == harmonic_records_per_family) {
		update.completed = std::move(partial_->snapshot);
		last_completed_sequence_ = chunk.sequence;
		partial_.reset();
	}
	return update;
}

void HarmonicFamilyAssembler::reset()
{
	partial_.reset();
	last_completed_sequence_.reset();
}

} // namespace msap1
