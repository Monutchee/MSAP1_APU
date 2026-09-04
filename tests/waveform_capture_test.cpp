#include "msap1/waveform/waveform_capture.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
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
		      std::uint32_t sample_rate_hz = 32000u,
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
	block.header.measured_sample_rate_hz = sample_rate_hz;
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

msap1::WaveformStatus wait_for_archive_discovery(
	msap1::WaveformCapture &capture)
{
	for (unsigned int attempt = 0; attempt < 1000; ++attempt) {
		auto status = capture.status();
		if (status.archive_discovery.state ==
		    msap1::WaveformArchiveDiscoveryState::complete)
			return status;
		if (status.archive_discovery.state ==
			    msap1::WaveformArchiveDiscoveryState::failed ||
		    status.archive_discovery.state ==
			    msap1::WaveformArchiveDiscoveryState::cancelled)
			throw std::runtime_error(
				"waveform archive discovery did not complete");
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	throw std::runtime_error("waveform archive discovery timed out");
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

msap1::WaveformSessionSummary find_session(
	const std::vector<msap1::WaveformSessionSummary> &sessions,
	std::uint64_t id, const std::string &message)
{
	const auto found = std::ranges::find_if(sessions,
		[id](const auto &session) { return session.id == id; });
	require(found != sessions.end(), message);
	return *found;
}

void test_session_pagination_and_origin_filters()
{
	const auto source_bit = [](msap1::WaveformTriggerSource source) {
		return 1u << static_cast<unsigned>(source);
	};
	const auto manual = source_bit(
		msap1::WaveformTriggerSource::manual_web);
	const auto power_quality = source_bit(
		msap1::WaveformTriggerSource::pq_event);

	std::vector<msap1::WaveformSessionSummary> sessions;
	for (std::uint64_t id = 1u; id <= 25u; ++id) {
		msap1::WaveformSessionSummary session{};
		session.id = id;
		session.state = id == 25u
			? msap1::WaveformSessionState::capturing
			: id == 24u
				? msap1::WaveformSessionState::incomplete
				: msap1::WaveformSessionState::complete;
		if (id == 20u)
			session.trigger_source_mask = manual | power_quality;
		else if ((id % 3u) == 0u)
			session.trigger_source_mask = power_quality;
		else if ((id % 2u) == 0u)
			session.trigger_source_mask = manual;
		/* Odd, non-PQ sessions deliberately model legacy/unknown files. */
		sessions.push_back(session);
	}

	msap1::WaveformSessionQuery query{};
	query.limit = 16u;
	const auto newest = msap1::waveform_session_page(sessions, query);
	require(newest.total_sessions == 25u &&
			newest.completed_sessions == 23u &&
			newest.incomplete_sessions == 1u &&
			newest.active_sessions == 1u &&
			newest.sessions.size() == 16u &&
			newest.sessions.front().id == 25u &&
			newest.sessions.back().id == 10u &&
			newest.next_before_session_id == 10u,
		"newest waveform page metadata is wrong");

	query.before_session_id = newest.next_before_session_id;
	const auto older = msap1::waveform_session_page(sessions, query);
	require(older.sessions.size() == 9u &&
			older.sessions.front().id == 9u &&
			older.sessions.back().id == 1u &&
			older.next_before_session_id == 0u,
		"exclusive waveform cursor skipped or duplicated a session");

	/* A newer capture cannot perturb an already-issued exclusive cursor. */
	msap1::WaveformSessionSummary added{};
	added.id = 26u;
	sessions.push_back(added);
	const auto stable = msap1::waveform_session_page(sessions, query);
	require(stable.sessions.size() == older.sessions.size() &&
			std::ranges::equal(stable.sessions, older.sessions,
				{}, &msap1::WaveformSessionSummary::id,
				&msap1::WaveformSessionSummary::id),
		"new capture changed an older waveform page");

	query.before_session_id = 0u;
	query.limit = 100u;
	query.origin = msap1::WaveformOriginFilter::manual;
	const auto manual_page = msap1::waveform_session_page(sessions, query);
	query.origin = msap1::WaveformOriginFilter::power_quality;
	const auto pq_page = msap1::waveform_session_page(sessions, query);
	const auto contains = [](const auto &page, std::uint64_t id) {
		return std::ranges::any_of(page.sessions,
			[id](const auto &session) { return session.id == id; });
	};
	require(contains(manual_page, 20u) && contains(pq_page, 20u),
		"mixed waveform session is not in both origin filters");
	require(!contains(manual_page, 1u) && !contains(pq_page, 1u),
		"legacy waveform session leaked into an origin filter");
	require(std::ranges::all_of(manual_page.sessions,
			[manual](const auto &session) {
				return (session.trigger_source_mask & manual) != 0u;
			}) &&
			std::ranges::all_of(pq_page.sessions,
				[power_quality](const auto &session) {
					return (session.trigger_source_mask &
						power_quality) != 0u;
				}),
		"waveform origin filtering returned a nonmatching session");

	bool rejected = false;
	try {
		query.limit = 0u;
		(void)msap1::waveform_session_page(sessions, query);
	} catch (const std::invalid_argument &) {
		rejected = true;
	}
	require(rejected, "zero waveform page limit was accepted");
	query.limit = 1u;
	query.origin = static_cast<msap1::WaveformOriginFilter>(99u);
	rejected = false;
	try {
		(void)msap1::waveform_session_page(sessions, query);
	} catch (const std::invalid_argument &) {
		rejected = true;
	}
	require(rejected, "unknown waveform origin filter was accepted");
}

void test_async_archive_discovery_and_cancellation()
{
	const auto device = unique_path(".archive-device");
	const auto output = unique_path(".archive-captures");
	const auto cancel_device = unique_path(".archive-cancel-device");
	const auto cancel_output = unique_path(".archive-cancel-captures");
	std::mutex gate_mutex;
	std::condition_variable gate;
	bool entered = false;
	bool release = false;
	std::atomic<bool> cancelled_hook_exited{false};
	try {
		std::filesystem::create_directories(output);
		{
			std::ofstream malformed(
				output / "waveform-41-blocked.mncwf",
				std::ios::binary | std::ios::trunc);
			malformed << "not an MNCWF file";
		}
		write_test_block(device);
		msap1::WaveformCaptureOptions options{};
		options.history_capacity_frames = 70'000u;
		options.archive_discovery_hook =
			[&](std::stop_token stop, const std::filesystem::path &) {
				std::unique_lock lock(gate_mutex);
				entered = true;
				gate.notify_all();
				std::stop_callback notify_stop(stop,
					[&gate] { gate.notify_all(); });
				gate.wait(lock, [&] {
					return release || stop.stop_requested();
				});
			};

		msap1::WaveformCapture capture(device.string(), output,
			test_capture_context(), options);
		capture.start();
		{
			std::unique_lock lock(gate_mutex);
			require(gate.wait_for(lock, std::chrono::seconds(1),
				[&] { return entered; }),
				"archive validation worker did not reach the test gate");
		}
		capture.read_available();
		const auto scanning = capture.status();
		require(scanning.running != 0u &&
				scanning.frames == msap1::waveform_frames_per_block &&
				scanning.archive_discovery.state ==
					msap1::WaveformArchiveDiscoveryState::scanning &&
				scanning.archive_discovery.scanned_files == 0u &&
				scanning.archive_discovery.total_files == 1u,
			"capture did not run while archive validation was blocked");
		const auto created = capture.trigger(
			0u, 0u, 1u, msap1::WaveformTriggerSource::manual_cli);
		require(created.id == 42u,
			"persisted filename did not reserve the next session ID");
		{
			std::scoped_lock lock(gate_mutex);
			release = true;
		}
		gate.notify_all();
		const auto complete = wait_for_archive_discovery(capture);
		require(complete.archive_discovery.scanned_files == 1u &&
				complete.archive_discovery.rejected_files == 1u,
			"malformed archive was not counted as rejected");
		require(find_session(capture.sessions(), 42u,
				"capture created during discovery disappeared").id == 42u,
			"runtime session collided with archive discovery");
		capture.stop();

		std::filesystem::create_directories(cancel_output);
		{
			std::ofstream malformed(
				cancel_output / "waveform-7-cancel.mncwf",
				std::ios::binary | std::ios::trunc);
			malformed << "not an MNCWF file";
		}
		{
			std::ofstream empty(cancel_device, std::ios::binary);
			require(static_cast<bool>(empty),
				"create archive cancellation device");
		}
		entered = false;
		release = false;
		options.archive_discovery_hook =
			[&](std::stop_token stop, const std::filesystem::path &) {
				std::unique_lock lock(gate_mutex);
				entered = true;
				gate.notify_all();
				std::stop_callback notify_stop(stop,
					[&gate] { gate.notify_all(); });
				gate.wait(lock, [&] { return stop.stop_requested(); });
				cancelled_hook_exited = true;
			};
		{
			msap1::WaveformCapture cancelling(cancel_device.string(),
				cancel_output, test_capture_context(), options);
			cancelling.start();
			std::unique_lock lock(gate_mutex);
			require(gate.wait_for(lock, std::chrono::seconds(1),
				[&] { return entered; }),
				"cancellable archive worker did not start");
		}
		require(cancelled_hook_exited.load(),
			"waveform shutdown did not cancel archive discovery");

		std::filesystem::remove_all(output);
		std::filesystem::remove_all(cancel_output);
		std::filesystem::remove(device);
		std::filesystem::remove(cancel_device);
	} catch (...) {
		{
			std::scoped_lock lock(gate_mutex);
			release = true;
		}
		gate.notify_all();
		std::filesystem::remove_all(output);
		std::filesystem::remove_all(cancel_output);
		std::filesystem::remove(device);
		std::filesystem::remove(cancel_device);
		throw;
	}
}

void test_archive_retention_and_safe_exclusions()
{
	const auto device = unique_path(".retention-device");
	const auto output = unique_path(".retention-captures");
	const auto blocked_device = unique_path(".retention-blocked-device");
	const auto blocked_output = unique_path(".retention-blocked-captures");
	try {
		write_test_block(device);
		msap1::WaveformCaptureOptions options{};
		options.history_capacity_frames = 70'000u;
		options.archive_limit_bytes = 1024u * 1024u;

		msap1::WaveformSessionSummary first{};
		std::filesystem::path first_path;
		std::uint64_t first_bytes = 0u;
		{
			msap1::WaveformCapture capture(device.string(), output,
				test_capture_context(), options);
			capture.start();
			(void)wait_for_archive_discovery(capture);
			capture.read_available();
			first = capture.trigger(0u, 0u, 1u,
				msap1::WaveformTriggerSource::manual_cli);
			capture.read_available();
			first = find_session(wait_for_session(capture, first.id), first.id,
				"first retention fixture disappeared");
			first_path = output / first.filename.data();
			first_bytes = std::filesystem::file_size(first_path);
			require(first.state == msap1::WaveformSessionState::complete &&
					first_bytes > 1024u,
				"first retention fixture did not materialize");
			capture.stop();
		}

		options.archive_limit_bytes = first_bytes + 1024u;
		{
			msap1::WaveformCapture capture(device.string(), output,
				test_capture_context(), options);
			capture.start();
			(void)wait_for_archive_discovery(capture);
			capture.read_available();
			auto newest = capture.trigger(0u, 0u, 1u,
				msap1::WaveformTriggerSource::manual_cli);
			capture.read_available();
			newest = find_session(wait_for_session(capture, newest.id), newest.id,
				"new retention fixture disappeared");
			const auto status = capture.status();
			require(newest.state == msap1::WaveformSessionState::complete &&
					!std::filesystem::exists(first_path) &&
					std::filesystem::exists(output / newest.filename.data()) &&
					status.expired_sessions == 1u &&
					status.archive_stored_bytes <= status.archive_limit_bytes,
				"retention did not expire the oldest valid completed capture");
			capture.stop();
		}

		write_test_block(blocked_device);
		std::filesystem::create_directories(blocked_output);
		const auto malformed_path =
			blocked_output / "waveform-40-malformed.mncwf";
		{
			std::ofstream malformed(malformed_path,
				std::ios::binary | std::ios::trunc);
			malformed << "not an MNCWF file";
		}
		const auto malformed_bytes = std::filesystem::file_size(malformed_path);
		options.archive_limit_bytes = malformed_bytes + first_bytes - 1u;
		{
			msap1::WaveformCapture capture(blocked_device.string(), blocked_output,
				test_capture_context(), options);
			capture.start();
			(void)wait_for_archive_discovery(capture);
			capture.read_available();
			auto rejected = capture.trigger(0u, 0u, 1u,
				msap1::WaveformTriggerSource::manual_cli);
			capture.read_available();
			rejected = find_session(wait_for_session(capture, rejected.id),
				rejected.id, "quota-rejected session disappeared");
			const auto status = capture.status();
			require(rejected.state == msap1::WaveformSessionState::incomplete &&
					std::filesystem::exists(malformed_path) &&
					status.retention_failures >= 1u &&
					std::ranges::distance(
						std::filesystem::directory_iterator(blocked_output),
						std::filesystem::directory_iterator{}) == 1,
				"unsafe retention candidate was removed or final commit succeeded");
			capture.stop();
		}

		std::filesystem::remove_all(output);
		std::filesystem::remove_all(blocked_output);
		std::filesystem::remove(device);
		std::filesystem::remove(blocked_device);
	} catch (...) {
		std::filesystem::remove_all(output);
		std::filesystem::remove_all(blocked_output);
		std::filesystem::remove(device);
		std::filesystem::remove(blocked_device);
		throw;
	}
}

void test_bounded_onset_and_restart_recovery()
{
	const auto device = unique_path(".continuation-device");
	const auto output = unique_path(".continuation-captures");
	const auto discovery_device = unique_path(".continuation-discovery");
	const auto recovery_device = unique_path(".recovery-device");
	const auto recovery_output = unique_path(".recovery-captures");
	constexpr std::uint32_t test_sample_rate_hz = 128'000u;
	constexpr std::size_t test_history_frames =
		2u * test_sample_rate_hz + 2'048u;
	msap1::WaveformCaptureOptions options{};
	options.history_capacity_frames = test_history_frames;
	try {
		msap1::WaveformSessionSummary master{};
		msap1::WaveformSessionSummary recovery_window{};
		{
			write_test_block(device, 1u, false, test_sample_rate_hz);
			msap1::WaveformCapture capture(device.string(), output,
				test_capture_context(), options);
			capture.start();
			capture.read_available();
			const auto initial = capture.status();
			require(initial.sample_rate_hz == test_sample_rate_hz &&
					initial.history_capacity_frames == test_history_frames &&
					initial.max_capture_frames == 2'048u,
				"default-rate continuation fixture has the wrong history budget");

			const msap1::WaveformEventIdentity event_id{21u, 1u};
			master = capture.track_power_quality_event(
				event_id, msap1::WaveformEventLifecycle::start,
				1024u, 1024u, 0u, 0u, 1u,
				pq_descriptor(event_id,
					msap1::WaveformEventLifecycle::start, 1024u, 1024u));
			const auto update = capture.track_power_quality_event(
				event_id, msap1::WaveformEventLifecycle::update,
				1024u, 3072u, 0u, 0u, 1u,
				pq_descriptor(event_id,
					msap1::WaveformEventLifecycle::update, 1024u, 3072u));
			require(master.association_created &&
					update.id == master.id && !update.association_created &&
					update.first_sequence == 1024u &&
					update.last_sequence == 1024u,
				"PQ UPDATE extended or duplicated the fixed onset window");

			write_test_block(device, 1025u, true, test_sample_rate_hz);
			write_test_block(device, 2049u, true, test_sample_rate_hz);
			write_test_block(device, 3073u, true, test_sample_rate_hz);
			capture.read_available();
			recovery_window = capture.track_power_quality_event(
				event_id, msap1::WaveformEventLifecycle::end,
				1024u, 4096u, 0u, 0u, 1u,
				pq_descriptor(event_id,
					msap1::WaveformEventLifecycle::end, 1024u, 4096u));
			capture.read_available();
			const auto completed_master = find_session(
				wait_for_session(capture, master.id), master.id,
				"materialized continuation master disappeared");
			const auto completed_recovery = find_session(
				wait_for_session(capture, recovery_window.id),
				recovery_window.id, "materialized recovery window disappeared");
			require(completed_master.state ==
					msap1::WaveformSessionState::complete &&
					completed_recovery.state ==
						msap1::WaveformSessionState::complete &&
					completed_master.first_sequence == 1024u &&
					completed_master.last_sequence == 1024u &&
					completed_recovery.first_sequence == 4096u &&
					completed_recovery.last_sequence == 4096u &&
					recovery_window.association_created,
				"onset and terminal recovery did not materialize as fixed windows");
			master = completed_master;
			recovery_window = completed_recovery;
			capture.stop();
		}

		const auto master_bytes = read_bytes(output / master.filename.data());
		const auto recovery_bytes = read_bytes(
			output / recovery_window.filename.data());
		const msap1::MncwfV4Reader master_reader(master_bytes);
		const msap1::MncwfV4Reader recovery_reader(recovery_bytes);
		require(master_reader.version() == msap1::mncwf_v5_version &&
				recovery_reader.version() == msap1::mncwf_v5_version &&
				master_reader.sample_frame_count() == 1u &&
				recovery_reader.sample_frame_count() == 1u &&
				master_reader.capture_metadata().capture_uuid ==
					master.capture_uuid &&
				recovery_reader.capture_metadata().capture_uuid ==
					recovery_window.capture_uuid,
			"materialized onset/recovery spans or UUIDs are wrong");
		require(master_reader.events().size() == 1u &&
			recovery_reader.events().size() == 1u &&
			master_reader.events().front().event_uuid ==
				recovery_reader.events().front().event_uuid,
			"bounded event windows lost stable event lineage");

		{
			std::ofstream empty(discovery_device, std::ios::binary);
			require(static_cast<bool>(empty),
				"create continuation discovery device");
		}
		{
			msap1::WaveformCapture discovered(discovery_device.string(), output,
				test_capture_context(), options);
			discovered.start();
			(void)wait_for_archive_discovery(discovered);
			const auto restored = discovered.sessions();
			const auto restored_master = find_session(restored, master.id,
				"restart did not discover continuation master");
			const auto restored_recovery = find_session(
				restored, recovery_window.id,
				"restart did not discover recovery window");
			require(restored_master.master_session_id == master.id &&
					restored_recovery.continuation_of_session_id == 0u &&
					restored_recovery.master_session_id ==
						recovery_window.id &&
					(restored_master.trigger_source_mask &
						(1u << static_cast<unsigned>(
							msap1::WaveformTriggerSource::pq_event))) != 0u &&
					(restored_recovery.trigger_source_mask &
						(1u << static_cast<unsigned>(
							msap1::WaveformTriggerSource::pq_event))) != 0u,
				"restart did not reconstruct bounded PQ capture metadata");
			discovered.stop();
		}

		{
			write_test_block(recovery_device, 50'000u, false,
				test_sample_rate_hz);
			msap1::WaveformCapture recovery(recovery_device.string(),
				recovery_output, test_capture_context(), options);
			recovery.start();
			recovery.read_available();
			const msap1::WaveformEventIdentity recovered_event{22u, 1u};
			const auto recovered = recovery.track_power_quality_event(
				recovered_event, msap1::WaveformEventLifecycle::update,
				1u, 51'023u, 0u, 0u, 1u,
				pq_descriptor(recovered_event,
					msap1::WaveformEventLifecycle::update, 1u, 51'023u));
			require(recovered.association_created &&
					recovered.first_sequence == 51'023u &&
					recovered.last_sequence == 51'023u,
				"UPDATE-first recovery was not bounded at the observed edge");
			const auto terminal = recovery.track_power_quality_event(
				recovered_event, msap1::WaveformEventLifecycle::end,
				1u, 51'023u, 0u, 0u, 1u,
				pq_descriptor(recovered_event,
					msap1::WaveformEventLifecycle::end, 1u, 51'023u));
			require(terminal.id == recovered.id &&
					!terminal.association_created,
				"terminal edge duplicated an UPDATE-first recovery capture");
			recovery.read_available();
			const auto completed = find_session(
				wait_for_session(recovery, recovered.id), recovered.id,
				"UPDATE-first recovery session disappeared");
			require(completed.state ==
					msap1::WaveformSessionState::complete,
				"UPDATE-first recovery tail was not materialized");
			const auto recovered_bytes = read_bytes(
				recovery_output / completed.filename.data());
			const msap1::MncwfV4Reader recovered_reader(recovered_bytes);
			require(recovered_reader.sample_frame_count() == 1u &&
					recovered_reader.events().size() == 1u &&
					(recovered_reader.events().front().flags &
						msap1::mncwf_event_discontinuous) != 0u &&
					(recovered_reader.events().front().flags &
						msap1::mncwf_event_trigger_valid) == 0u,
				"UPDATE-first recovery did not persist a clipped diagnostic tail");
			recovery.stop();
		}

		std::filesystem::remove_all(output);
		std::filesystem::remove_all(recovery_output);
		std::filesystem::remove(device);
		std::filesystem::remove(discovery_device);
		std::filesystem::remove(recovery_device);
	} catch (...) {
		std::filesystem::remove_all(output);
		std::filesystem::remove_all(recovery_output);
		std::filesystem::remove(device);
		std::filesystem::remove(discovery_device);
		std::filesystem::remove(recovery_device);
		throw;
	}
}

} // namespace

int main()
{
	const auto device = unique_path(".device");
	const auto output = unique_path(".captures");
	try {
		test_session_pagination_and_origin_filters();
		test_async_archive_discovery_and_cancellation();
		test_archive_retention_and_safe_exclusions();
		test_bounded_onset_and_restart_recovery();
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
		require((triggered.trigger_source_mask &
				(1u << static_cast<unsigned>(
					msap1::WaveformTriggerSource::manual_cli))) != 0u,
			"manual capture origin was not retained");

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

		/* UPDATE never extends an onset capture. A nearby END merges its fixed
		 * recovery window while the onset has not yet been queued. */
		const msap1::WaveformEventIdentity pq_one{11, 1};
		const auto pq_started = capture.track_power_quality_event(
			pq_one, msap1::WaveformEventLifecycle::start,
			1000, 1024, 1, 0, 1,
			pq_descriptor(pq_one, msap1::WaveformEventLifecycle::start,
				1000, 1024));
		require(pq_started.association_created &&
				(pq_started.trigger_source_mask &
				(1u << static_cast<unsigned>(
					msap1::WaveformTriggerSource::pq_event))) != 0u,
			"PQ capture origin was not retained");
		const auto pq_update = capture.track_power_quality_event(
			pq_one, msap1::WaveformEventLifecycle::update,
			1000, 100000, 1, 0, 1,
			pq_descriptor(pq_one, msap1::WaveformEventLifecycle::update,
				1000, 100000));
		require(pq_update.id == pq_started.id &&
				pq_update.first_sequence == pq_started.first_sequence &&
				pq_update.last_sequence == pq_started.last_sequence &&
				!pq_update.association_created,
			"PQ UPDATE extended or duplicated the onset capture");
		const auto pq_terminal = capture.track_power_quality_event(
			pq_one, msap1::WaveformEventLifecycle::end,
			1000, 1024, 1, 0, 1,
			pq_descriptor(pq_one, msap1::WaveformEventLifecycle::end,
				1000, 1024));
		require(pq_terminal.id == pq_started.id &&
				!pq_terminal.association_created &&
				pq_terminal.first_sequence == 968u &&
				pq_terminal.last_sequence == 1024u,
			"overlapping onset and recovery windows were not merged");
		capture.read_available();
		auto pq_sessions = wait_for_session(capture, pq_started.id);
		auto pq_session = std::find_if(pq_sessions.begin(), pq_sessions.end(),
			[&](const auto &session) { return session.id == pq_started.id; });
		require(pq_session != pq_sessions.end() &&
				pq_session->state == msap1::WaveformSessionState::complete &&
				pq_session->event_count == 1 &&
				pq_session->master_session_id == pq_session->id &&
				pq_session->continuation_of_session_id == 0,
			"the completed PQ capture lost its bounded identity");
		const auto pq_file = output / pq_session->filename.data();
		const auto pq_bytes = read_bytes(pq_file);
		const msap1::MncwfV4Reader pq_reader(pq_bytes);
		require(pq_reader.events().size() == 1u,
			"the MNCWF capture lost its event descriptor");
		require(std::ranges::count_if(pq_reader.lineage(), [](const auto &entry) {
			return entry.relation == msap1::MncwfLineageRelation::event;
		}) == 1,
			"the MNCWF master lost event UUID lineage");
		capture.erase(pq_started.id);

		/* A long UPDATE remains one onset. Once it materializes, END creates
		 * exactly one bounded recovery association, never continuations. */
		const msap1::WaveformEventIdentity pq_long{11, 3};
		const auto long_onset = capture.track_power_quality_event(
			pq_long, msap1::WaveformEventLifecycle::start,
			1024, 1024, 0, 0, 1,
			pq_descriptor(pq_long, msap1::WaveformEventLifecycle::start,
				1024, 1024));
		const auto safe_frames = capture.status().max_capture_frames;
		const auto ignored_update = capture.track_power_quality_event(
			pq_long, msap1::WaveformEventLifecycle::update,
			1024, 1024 + safe_frames + 10, 0, 0, 1,
			pq_descriptor(pq_long, msap1::WaveformEventLifecycle::update,
				1024, 1024 + safe_frames + 10));
		require(ignored_update.id == long_onset.id &&
				ignored_update.last_sequence == 1024u &&
				!ignored_update.association_created,
			"long UPDATE created a continuation");
		capture.read_available();
		(void)wait_for_session(capture, long_onset.id);
		const auto long_recovery = capture.track_power_quality_event(
			pq_long, msap1::WaveformEventLifecycle::end,
			1024, 1024, 0, 0, 1,
			pq_descriptor(pq_long, msap1::WaveformEventLifecycle::end,
				1024, 1024));
		require(long_recovery.id != long_onset.id &&
				long_recovery.association_created &&
				long_recovery.continuation_of_session_id == 0u &&
				long_recovery.first_sequence == 1024u &&
				long_recovery.last_sequence == 1024u,
			"terminal event did not create one bounded recovery window");
		capture.erase(long_onset.id);

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
		(void)wait_for_archive_discovery(restarted);
		const auto restored = restarted.sessions();
		require(!restored.empty() &&
				restored.front().id == triggered.id &&
				restored.front().state ==
					msap1::WaveformSessionState::complete &&
				restored.front()
						.trigger_realtime_nanoseconds != 0,
			"persisted waveform history was not restored");
		const auto found_by_id = restarted.find_session(triggered.id);
		const auto found_by_uuid =
			restarted.find_session(restored.front().capture_uuid);
		require(found_by_id && found_by_uuid &&
				found_by_id->capture_uuid == restored.front().capture_uuid &&
				found_by_uuid->id == triggered.id,
			"exact waveform archive lookup did not find an old capture");
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
