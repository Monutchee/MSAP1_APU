#include "msap1/waveform/waveform_capture.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace {

void require(bool condition, const std::string &message)
{
	if (!condition)
		throw std::runtime_error(message);
}

std::filesystem::path unique_path(const std::string &suffix)
{
	return std::filesystem::temp_directory_path() /
		("msap1-waveform-test-" + std::to_string(::getpid()) + suffix);
}

void write_test_block(const std::filesystem::path &path,
		      std::uint64_t first_sequence = 1,
		      bool append = false,
		      std::uint32_t configuration_generation = 0x12345678u)
{
	msap1::WaveformBlock block{};
	block.header.magic = msap1::waveform_block_magic;
	block.header.version = msap1::waveform_block_version;
	block.header.block_bytes = msap1::waveform_block_bytes;
	block.header.frame_count = msap1::waveform_frames_per_block;
	block.header.frame_bytes = msap1::waveform_frame_bytes;
	block.header.first_sequence_low =
		static_cast<std::uint32_t>(first_sequence);
	block.header.first_sequence_high =
		static_cast<std::uint32_t>(first_sequence >> 32u);
	block.header.first_tick_low = 100;
	block.header.measured_sample_rate_hz = 32000;
	block.header.configuration_generation = configuration_generation;
	block.header.block_sequence = static_cast<std::uint32_t>(
		(first_sequence - 1u) / msap1::waveform_frames_per_block + 1u);
	for (std::size_t frame = 0; frame < block.frames.size(); ++frame) {
		for (std::size_t channel = 0;
		     channel < block.frames[frame].size(); ++channel) {
			block.frames[frame][channel] =
				static_cast<std::int32_t>(frame * 10 + channel);
		}
	}

	std::ofstream stream(path, std::ios::binary |
		(append ? std::ios::app : std::ios::trunc));
	require(static_cast<bool>(stream), "create synthetic waveform device");
	stream.write(reinterpret_cast<const char *>(&block), sizeof(block));
	require(static_cast<bool>(stream), "write synthetic waveform block");
}

std::vector<msap1::WaveformSessionSummary> wait_for_session(
	msap1::WaveformCapture &capture, std::uint64_t id)
{
	for (unsigned int attempt = 0; attempt < 200; ++attempt) {
		auto sessions = capture.sessions();
		const auto found = std::find_if(
			sessions.begin(), sessions.end(),
			[id](const auto &session) { return session.id == id; });
		if (found != sessions.end() &&
		    found->state != msap1::WaveformSessionState::capturing)
			return sessions;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	throw std::runtime_error("waveform materialization timed out");
}

msap1::WaveformCaptureContext test_capture_context()
{
	msap1::WaveformCaptureContext context{};
	for (std::size_t index = 0; index < 16u; ++index)
		context.capture_metadata.device_uuid[index] =
			static_cast<std::byte>(0x20u + index);
	for (std::size_t index = 0; index < 32u; ++index) {
		context.capture_metadata.configuration_sha256[index] =
			static_cast<std::byte>(0x40u + index);
		context.capture_metadata.sensor_profile_sha256[index] =
			static_cast<std::byte>(0x80u + index);
	}
	context.capture_metadata.nominal_voltage_numerator = 120;
	context.capture_metadata.nominal_voltage_denominator = 1;
	context.capture_metadata.nominal_frequency_numerator = 60;
	context.capture_metadata.nominal_frequency_denominator = 1;
	context.capture_metadata.topology = msap1::MncwfTopology::wye;
	context.capture_metadata.calibration_status =
		msap1::MncwfCalibrationStatus::valid;
	context.capture_metadata.station_name = "test station";
	context.capture_metadata.site_name = "test site";
	context.capture_metadata.circuit_name = "test circuit";
	context.capture_metadata.product_name = "MSAP1";
	context.capture_metadata.device_model = "MSAP1-test";
	context.capture_metadata.firmware_version = "test-firmware";
	context.capture_metadata.software_build_id = "test-build";
	context.capture_metadata.sensor_profile_id = "test-profile";
	context.capture_metadata.configuration_id = "test-configuration";
	context.capture_metadata.calibration_id = "test-calibration";
	context.capture_metadata.device_serial = "test-device";
	static constexpr std::array<const char *, msap1::waveform_persisted_channels>
		names{"Ia", "Ib", "Ic", "In", "Vc", "Vb", "Va"};
	static constexpr std::array phases{
		msap1::MncwfPhase::a, msap1::MncwfPhase::b,
		msap1::MncwfPhase::c, msap1::MncwfPhase::neutral,
		msap1::MncwfPhase::c, msap1::MncwfPhase::b,
		msap1::MncwfPhase::a,
	};
	for (std::size_t index = 0; index < names.size(); ++index) {
		msap1::MncwfV4ChannelDefinition channel{};
		for (std::size_t byte = 0; byte < channel.stable_id.size(); ++byte)
			channel.stable_id[byte] = static_cast<std::byte>(
				0xa0u + index * 16u + byte);
		channel.source_channel = index;
		channel.flags = msap1::mncwf_channel_enabled |
			msap1::mncwf_channel_transform_valid |
			msap1::mncwf_channel_ratio_valid |
			msap1::mncwf_channel_nominal_valid |
			msap1::mncwf_channel_range_valid |
			msap1::mncwf_channel_resolution_valid |
			msap1::mncwf_channel_clipping_valid |
			msap1::mncwf_channel_calibration_valid;
		channel.phase = phases[index];
		channel.quantity = index < 4u ? msap1::MncwfQuantity::current
			: msap1::MncwfQuantity::voltage;
		channel.si_unit = index < 4u ? msap1::MncwfSiUnit::ampere
			: msap1::MncwfSiUnit::volt;
		channel.storage_bits = 32;
		channel.valid_bits = 24;
		channel.display_exponent10 = -6;
		channel.gain_numerator = static_cast<std::int64_t>(1000u + index);
		channel.gain_denominator = 65'536'000'000ull;
		channel.offset_denominator = 1;
		channel.primary_secondary_ratio_numerator = 1;
		channel.primary_secondary_ratio_denominator = 1;
		channel.nominal_numerator = index < 4u ? 5 : 120;
		channel.nominal_denominator = 1;
		channel.range_minimum_numerator = -1000;
		channel.range_minimum_denominator = 1;
		channel.range_maximum_numerator = 1000;
		channel.range_maximum_denominator = 1;
		channel.resolution_numerator = 1;
		channel.resolution_denominator = 1'000'000;
		channel.clipping_low = -(1ll << 23u);
		channel.clipping_high = (1ll << 23u) - 1;
		channel.name = names[index];
		channel.unit_symbol = index < 4u ? "A" : "V";
		channel.description = "test channel";
		context.channels.push_back(std::move(channel));
	}
	context.clock_source = msap1::MncwfClockSource::system;
	context.time_quality = msap1::MncwfTimeQuality::locked;
	context.time_flags = msap1::mncwf_time_utc_offset_known;
	context.utc_offset_seconds = -4 * 60 * 60;
	return context;
}

std::vector<std::byte> read_bytes(const std::filesystem::path &path)
{
	std::ifstream input(path, std::ios::binary);
	require(static_cast<bool>(input), "open MNCWF test file");
	input.seekg(0, std::ios::end);
	const auto end = input.tellg();
	require(end >= 0, "measure MNCWF test file");
	input.seekg(0, std::ios::beg);
	std::vector<std::byte> result(static_cast<std::size_t>(end));
	input.read(reinterpret_cast<char *>(result.data()),
		static_cast<std::streamsize>(result.size()));
	require(input.gcount() == static_cast<std::streamsize>(result.size()),
		"read complete MNCWF test file");
	return result;
}

std::int32_t read_s32(std::span<const std::byte> bytes, std::size_t offset)
{
	require(offset <= bytes.size() && bytes.size() - offset >= 4u,
		"read signed MNCWF sample");
	std::uint32_t raw = 0u;
	for (unsigned byte = 0; byte < 4u; ++byte)
		raw |= static_cast<std::uint32_t>(
			std::to_integer<std::uint8_t>(bytes[offset + byte]))
			<< (byte * 8u);
	return std::bit_cast<std::int32_t>(raw);
}

msap1::MncwfV4EventDescriptor pq_descriptor(
	msap1::WaveformEventIdentity identity,
	msap1::WaveformEventLifecycle lifecycle,
	std::uint64_t start_sequence, std::uint64_t current_sequence)
{
	msap1::MncwfV4EventDescriptor event{};
	for (unsigned byte = 0; byte < 8u; ++byte) {
		event.event_uuid[byte] = static_cast<std::byte>(
			(identity.session >> (byte * 8u)) & 0xffu);
		event.event_uuid[8u + byte] = static_cast<std::byte>(
			(identity.counter >> (byte * 8u)) & 0xffu);
	}
	event.event_uuid[6] = static_cast<std::byte>(
		(std::to_integer<std::uint8_t>(event.event_uuid[6]) & 0x0fu) |
		0x50u);
	event.event_uuid[8] = static_cast<std::byte>(
		(std::to_integer<std::uint8_t>(event.event_uuid[8]) & 0x3fu) |
		0x80u);
	event.taxonomy = msap1::MncwfEventTaxonomy::iec_61000_4_30;
	event.event_type = 1u;
	event.lifecycle = static_cast<msap1::MncwfEventLifecycle>(
		static_cast<std::uint16_t>(lifecycle) + 1u);
	event.time_quality = msap1::MncwfTimeQuality::locked;
	event.flags = msap1::mncwf_event_start_valid |
		msap1::mncwf_event_current_valid |
		msap1::mncwf_event_trigger_valid |
		msap1::mncwf_event_settings_snapshot_valid;
	if (lifecycle == msap1::WaveformEventLifecycle::end ||
	    lifecycle == msap1::WaveformEventLifecycle::abort)
		event.flags |= msap1::mncwf_event_end_valid;
	event.phase_mask = msap1::mncwf_event_phase_a;
	event.quantity = msap1::MncwfQuantity::voltage;
	event.si_unit = msap1::MncwfSiUnit::volt;
	event.trigger_source = 3u;
	event.configuration_generation = 7u;
	event.start_sequence = start_sequence;
	event.current_sequence = current_sequence;
	event.end_sequence = current_sequence;
	event.trigger_sequence = start_sequence;
	event.reference_micro_units = 120'000'000;
	event.threshold_micro_units = 108'000'000;
	event.hysteresis_micro_units = 2'400'000;
	event.extrema_micro_units = {90'000'000, 120'000'000, 120'000'000};
	event.duration_samples = current_sequence - start_sequence;
	event.update_count = 1u;
	event.taxonomy_name = "IEC 61000-4-30 voltage event";
	event.label = "voltage sag";
	event.settings_snapshot_json =
		R"({"threshold_e4":9000,"hysteresis_e4":200})";
	return event;
}

} // namespace

int main()
{
	const auto device = unique_path(".device");
	const auto output = unique_path(".captures");
	try {
		write_test_block(device);
		msap1::WaveformCapture capture(
			device.string(), output, test_capture_context());
		capture.start();
		capture.read_available();

		const auto initial = capture.status();
		require(initial.running != 0, "capture did not open");
		require(initial.blocks == 1, "block counter mismatch");
		require(initial.frames == msap1::waveform_frames_per_block,
			"frame counter mismatch");
		require(initial.history_oldest_sequence == 1,
			"oldest sequence mismatch");
		require(initial.history_latest_sequence == 1024,
			"latest sequence mismatch");
		require(initial.pl_dropped_frames == 0,
			"PL drop counter should start clean");

		/*
		 * The capture budget is rate-derived: the frame-sized history
		 * ring minus two seconds of margin at the measured rate.
		 */
		require(initial.max_capture_frames ==
				msap1::waveform_history_frames - 2u * 32000u,
			"capture budget mismatch at 32 kSPS");
		bool budget_rejected = false;
		try {
			capture.trigger(120000, 120000, 1,
				msap1::WaveformTriggerSource::manual_cli);
		} catch (const std::invalid_argument &error) {
			/* The rejection must name the budget in ms. */
			budget_rejected =
				std::string(error.what()).find(" ms") !=
				std::string::npos;
		}
		require(budget_rejected,
			"over-budget trigger was not rejected with the limit");
		require(capture.sessions().empty(),
			"a rejected trigger must not leave a session behind");

		const auto triggered = capture.trigger(
			10, 0, 1, msap1::WaveformTriggerSource::manual_cli);
		require(triggered.state ==
				msap1::WaveformSessionState::capturing,
			"trigger did not create an active session");
		require(triggered.first_sequence == 704,
			"pre-trigger frame calculation mismatch");
		require(triggered.last_sequence == 1024,
			"post-trigger frame calculation mismatch");

		/* EOF still queues session completion after the read loop. */
		capture.read_available();
		const auto sessions = wait_for_session(capture, triggered.id);
		require(sessions.size() == 1, "session was not retained");
		require(sessions.front().state ==
				msap1::WaveformSessionState::complete,
			"session was not materialized");
		require(sessions.front().event_count == 1,
			"event marker count mismatch");
		const auto capture_file = output / sessions.front().filename.data();
		require(std::filesystem::exists(capture_file),
			"capture file is missing");

		const auto persisted_bytes = read_bytes(capture_file);
		const msap1::MncwfV4Reader persisted(persisted_bytes);
		require(persisted.header().section_count ==
				msap1::mncwf_v4_mandatory_section_count,
			"capture did not emit every mandatory MNCWF v4 section");
		require(persisted.capture_metadata().capture_uuid ==
				sessions.front().capture_uuid,
			"persisted capture UUID mismatch");
		require(persisted.channels().size() ==
				msap1::waveform_persisted_channels &&
				persisted.sample_frame_bytes() == 28u,
			"persisted channel layout mismatch");
		require(persisted.channels().front().name == "Ia" &&
				persisted.channels().front().gain_numerator == 1000,
			"channel conversion metadata mismatch");
		require(persisted.timebase_segments().size() == 1u &&
				persisted.timebase_segments().front().decimation_divisor == 1u &&
				persisted.timebase_segments().front().source_frame_count == 321u,
			"undecimated timebase mismatch");
		require(persisted.sample_frame_count() == 321u,
			"undecimated sample count mismatch");
		require(persisted.events().size() == 1u &&
				persisted.events().front().lifecycle ==
					msap1::MncwfEventLifecycle::complete &&
				!persisted.events().front().settings_snapshot_json.empty(),
			"manual event descriptor mismatch");
		const auto readiness =
			msap1::assess_mncwf_v4_conversion_readiness(persisted);
		require(readiness.comtrade_ready() && readiness.pqdif_ready(),
			"production MNCWF v4 capture is not conversion-ready");
		const auto persisted_frame = persisted.sample_frame(0u);
		require(read_s32(persisted_frame, 0u) == 7030 &&
				read_s32(persisted_frame, 24u) == 7036,
			"persisted sample channel selection mismatch");
		const std::string capture_name =
			sessions.front().filename.data();
		require(capture_name.find("waveform-1-") == 0 &&
				capture_name.ends_with(".mncwf") &&
				capture_name.find('_') != std::string::npos,
			"human-readable capture filename mismatch");

		/*
		 * A decimated capture: 2 ms pre at 32 kSPS = 64 frames plus
		 * the anchor = a 65-frame window, folded by 4 into 17 stored
		 * frames whose values are the per-group means of the ramp.
		 */
		bool bad_decimation_rejected = false;
		try {
			capture.trigger(2, 0, 3,
				msap1::WaveformTriggerSource::manual_cli);
		} catch (const std::invalid_argument &) {
			bad_decimation_rejected = true;
		}
		require(bad_decimation_rejected,
			"decimation 3 must be rejected");
		const auto decimated = capture.trigger(
			2, 0, 4, msap1::WaveformTriggerSource::manual_cli);
		require(decimated.first_sequence == 960 &&
				decimated.last_sequence == 1024,
			"decimated window calculation mismatch");
		capture.read_available();
		const auto decimated_sessions =
			wait_for_session(capture, decimated.id);
		const auto decimated_session = std::find_if(
			decimated_sessions.begin(), decimated_sessions.end(),
			[&decimated](const auto &session) {
				return session.id == decimated.id;
			});
		require(decimated_session != decimated_sessions.end() &&
				decimated_session->state ==
					msap1::WaveformSessionState::complete,
			"decimated session was not materialized");
		require(decimated_session->decimation == 4u,
			"session decimation was not retained");
		const auto decimated_file =
			output / decimated_session->filename.data();
		const auto decimated_frames = (1024u - 960u) / 4u + 1u;
		const auto reduced_bytes = read_bytes(decimated_file);
		const msap1::MncwfV4Reader reduced(reduced_bytes);
		require(reduced.sample_frame_count() == decimated_frames &&
				reduced.timebase_segments().size() == 1u &&
				reduced.timebase_segments().front().decimation_divisor == 4u &&
				reduced.timebase_segments().front().sequence_step == 4u &&
				reduced.timebase_segments().front().source_frame_count == 65u,
			"decimated timebase mismatch");
		/*
		 * Sequences 960..963 are ramp values 9590..9620 step 10, so
		 * the stored group mean is 9605 plus the channel offset.
		 */
		const auto mean_frame = reduced.sample_frame(0u);
		require(read_s32(mean_frame, 0u) == 9605 &&
				read_s32(mean_frame, 24u) == 9611,
			"decimated samples are not the group means");
		/* Erase it so the restore test below still sees one file. */
		capture.erase(decimated.id);
		require(!std::filesystem::exists(decimated_file),
			"deleted decimated capture was retained");

		/* Stable PQ events share one capture union. Lifecycle updates extend
		 * it without duplicate markers, and a terminal edge cannot close the
		 * union while another overlapping event remains active. */
		const msap1::WaveformEventIdentity pq_one{11, 1};
		const msap1::WaveformEventIdentity pq_two{11, 2};
		const auto pq_started = capture.track_power_quality_event(
			pq_one, msap1::WaveformEventLifecycle::start,
			1000, 1024, 1, 0, 1,
			pq_descriptor(pq_one, msap1::WaveformEventLifecycle::start,
				1000, 1024));
		capture.read_available();
		auto pq_sessions = capture.sessions();
		auto pq_session = std::find_if(pq_sessions.begin(), pq_sessions.end(),
			[&](const auto &session) { return session.id == pq_started.id; });
		require(pq_session != pq_sessions.end() &&
				pq_session->state == msap1::WaveformSessionState::capturing,
			"an active PQ event did not hold its capture union open");
		const auto pq_overlap = capture.track_power_quality_event(
			pq_two, msap1::WaveformEventLifecycle::start,
			1005, 1024, 1, 0, 1,
			pq_descriptor(pq_two, msap1::WaveformEventLifecycle::start,
				1005, 1024));
		require(pq_overlap.id == pq_started.id && pq_overlap.event_count == 2,
			"overlapping PQ events did not merge into one master");
		const auto pq_update = capture.track_power_quality_event(
			pq_one, msap1::WaveformEventLifecycle::update,
			1000, 1024, 1, 0, 1,
			pq_descriptor(pq_one, msap1::WaveformEventLifecycle::update,
				1000, 1024));
		require(pq_update.event_count == 2,
			"PQ UPDATE added a duplicate event marker");
		(void)capture.track_power_quality_event(
			pq_one, msap1::WaveformEventLifecycle::end,
			1000, 1024, 1, 0, 1,
			pq_descriptor(pq_one, msap1::WaveformEventLifecycle::end,
				1000, 1024));
		capture.read_available();
		pq_sessions = capture.sessions();
		pq_session = std::find_if(pq_sessions.begin(), pq_sessions.end(),
			[&](const auto &session) { return session.id == pq_started.id; });
		require(pq_session->state == msap1::WaveformSessionState::capturing,
			"one terminal event closed a still-overlapping capture union");
		(void)capture.track_power_quality_event(
			pq_two, msap1::WaveformEventLifecycle::abort,
			1005, 1024, 1, 0, 1,
			pq_descriptor(pq_two, msap1::WaveformEventLifecycle::abort,
				1005, 1024));
		capture.read_available();
		pq_sessions = wait_for_session(capture, pq_started.id);
		pq_session = std::find_if(pq_sessions.begin(), pq_sessions.end(),
			[&](const auto &session) { return session.id == pq_started.id; });
		require(pq_session != pq_sessions.end() &&
				pq_session->state == msap1::WaveformSessionState::complete &&
				pq_session->event_count == 2 &&
				pq_session->master_session_id == pq_session->id &&
				pq_session->continuation_of_session_id == 0,
			"the completed PQ capture lost union or lineage identity");
		const auto pq_file = output / pq_session->filename.data();
		const auto pq_bytes = read_bytes(pq_file);
		const msap1::MncwfV4Reader pq_reader(pq_bytes);
		require(pq_reader.events().size() == 2u,
			"the MNCWF master lost an overlapping event descriptor");
		require(std::ranges::count_if(pq_reader.lineage(), [](const auto &entry) {
			return entry.relation == msap1::MncwfLineageRelation::event;
		}) == 2,
			"the MNCWF master lost event UUID lineage");
		capture.erase(pq_started.id);

		/* A live event longer than the safe materialization budget seals one
		 * master and carries the stable ID into a contiguous continuation. */
		const msap1::WaveformEventIdentity pq_long{11, 3};
		const auto long_master = capture.track_power_quality_event(
			pq_long, msap1::WaveformEventLifecycle::start,
			1024, 1024, 0, 0, 1,
			pq_descriptor(pq_long, msap1::WaveformEventLifecycle::start,
				1024, 1024));
		const auto safe_frames = capture.status().max_capture_frames;
		const auto long_continuation = capture.track_power_quality_event(
			pq_long, msap1::WaveformEventLifecycle::update,
			1024, 1024 + safe_frames + 10, 0, 0, 1,
			pq_descriptor(pq_long, msap1::WaveformEventLifecycle::update,
				1024, 1024 + safe_frames + 10));
		require(long_continuation.id != long_master.id &&
				long_continuation.continuation_of_session_id ==
					long_master.id &&
				long_continuation.master_session_id == long_master.id,
			"capacity continuation lost master/predecessor lineage");
		const auto long_sessions = capture.sessions();
		const auto sealed = std::find_if(long_sessions.begin(),
			long_sessions.end(), [&](const auto &session) {
				return session.id == long_master.id;
			});
		require(sealed != long_sessions.end() &&
				sealed->last_sequence + 1 ==
					long_continuation.first_sequence &&
				sealed->last_sequence - sealed->first_sequence + 1 ==
					safe_frames,
			"capacity continuation is not contiguous at the safe limit");

		/*
		 * A coordinated source change closes and reopens waveform DMA. The
		 * next source is a new continuity epoch even if its first sequence is
		 * unrelated to the previous source.
		 */
		capture.stop();
		write_test_block(device, 70000, false);
		capture.start();
		capture.read_available();
		const auto restarted_status = capture.status();
		require(restarted_status.sequence_gaps == 0,
			"source restart was reported as a sequence gap");
		require(restarted_status.history_oldest_sequence == 70000 &&
				restarted_status.history_latest_sequence == 71023,
			"source restart did not establish a fresh history epoch");

		/*
		 * A forward DMA discontinuity must be retained as a gap and must
		 * make any session spanning it incomplete rather than producing a
		 * corrupt file.
		 */
		write_test_block(device, 72048, true);
		capture.read_available();
		const auto gap_status = capture.status();
		require(gap_status.sequence_gaps == 1024,
			"missing frame count mismatch");
		const auto gap_triggered = capture.trigger(
			50, 0, 1, msap1::WaveformTriggerSource::manual_cli);
		capture.read_available();
		const auto gap_sessions = capture.sessions();
		const auto gap_session = std::find_if(
			gap_sessions.begin(), gap_sessions.end(),
			[&gap_triggered](const auto &session) {
				return session.id == gap_triggered.id;
			});
		require(gap_session != gap_sessions.end(),
			"gap session was not retained");
		require(gap_session->state ==
				msap1::WaveformSessionState::incomplete,
			"gap-crossing session was materialized");
		capture.stop();

		const auto empty_device = unique_path(".empty-device");
		std::ofstream(empty_device, std::ios::binary);
		msap1::WaveformCapture restarted(
			empty_device.string(), output, test_capture_context());
		restarted.start();
		const auto restored = restarted.sessions();
		require(!restored.empty() &&
				restored.front().id == triggered.id &&
				restored.front().state ==
					msap1::WaveformSessionState::complete &&
				restored.front()
						.trigger_realtime_nanoseconds != 0,
			"persisted waveform history was not restored");
		restarted.erase(triggered.id);
		require(restarted.sessions().empty(),
			"deleted waveform session was retained");
		require(!std::filesystem::exists(capture_file),
			"deleted waveform file was retained");
		restarted.stop();
		std::filesystem::remove(empty_device);
		std::filesystem::remove_all(output);
		std::filesystem::remove(device);
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "waveform_capture_test: " << error.what() << '\n';
		std::filesystem::remove_all(output);
		std::filesystem::remove(device);
		return 1;
	}
}
