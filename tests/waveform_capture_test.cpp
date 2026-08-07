#include "msap1/waveform/waveform_capture.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

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

} // namespace

int main()
{
	const auto device = unique_path(".device");
	const auto output = unique_path(".captures");
	try {
		write_test_block(device);
		std::array<msap1::WaveformChannelMetadata,
			   msap1::waveform_persisted_channels>
			metadata{};
		static constexpr std::array<const char *,
					    msap1::waveform_persisted_channels>
			names{"Ia", "Ib", "Ic", "In", "Vc", "Vb", "Va"};
		for (std::size_t channel = 0; channel < metadata.size();
		     ++channel) {
			metadata[channel].source_channel = channel;
			metadata[channel].kind =
				channel < 4
					? msap1::WaveformChannelKind::current
					: msap1::WaveformChannelKind::voltage;
			metadata[channel].scale_micro_units_q16 =
				static_cast<std::uint32_t>(1000 + channel);
			metadata[channel].flags = 1;
			std::copy_n(names[channel],
				    std::strlen(names[channel]),
				    metadata[channel].name.begin());
			metadata[channel].unit.front() =
				channel < 4 ? 'A' : 'V';
		}
		msap1::WaveformCapture capture(
			device.string(), output, metadata);
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

		const auto triggered = capture.trigger(
			10, 0, msap1::WaveformTriggerSource::manual_cli);
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

		const auto expected_size = 256u +
			msap1::waveform_persisted_channels *
				sizeof(msap1::WaveformChannelMetadata) +
			24u + (1024u - 704u + 1u) *
				msap1::waveform_persisted_channels *
				sizeof(std::int32_t);
		require(std::filesystem::file_size(capture_file) == expected_size,
			"capture file layout mismatch");
		std::ifstream persisted(capture_file, std::ios::binary);
		persisted.seekg(8);
		std::uint32_t file_version = 0;
		persisted.read(reinterpret_cast<char *>(&file_version),
			       sizeof(file_version));
		require(file_version == 2u, "capture file version mismatch");
		persisted.seekg(96);
		std::uint32_t channel_count = 0;
		std::uint32_t frame_bytes = 0;
		persisted.read(reinterpret_cast<char *>(&channel_count),
			       sizeof(channel_count));
		persisted.read(reinterpret_cast<char *>(&frame_bytes),
			       sizeof(frame_bytes));
		require(channel_count == 7u && frame_bytes == 28u,
			"persisted channel layout mismatch");
		persisted.seekg(256);
		msap1::WaveformChannelMetadata persisted_channel{};
		persisted.read(
			reinterpret_cast<char *>(&persisted_channel),
			sizeof(persisted_channel));
		require(std::string(persisted_channel.name.data()) == "Ia" &&
				persisted_channel.scale_micro_units_q16 == 1000u,
			"channel conversion metadata mismatch");
		persisted.seekg(256 +
			msap1::waveform_persisted_channels *
				sizeof(msap1::WaveformChannelMetadata) +
			24);
		std::array<std::int32_t,
			   msap1::waveform_persisted_channels>
			persisted_frame{};
		persisted.read(
			reinterpret_cast<char *>(persisted_frame.data()),
			sizeof(persisted_frame));
		require(persisted_frame[0] == 7030 &&
				persisted_frame[6] == 7036,
			"persisted sample channel selection mismatch");
		const std::string capture_name =
			sessions.front().filename.data();
		require(capture_name.find("waveform-1-") == 0 &&
				capture_name.ends_with(".mncwf") &&
				capture_name.find('_') != std::string::npos,
			"human-readable capture filename mismatch");

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
			50, 0, msap1::WaveformTriggerSource::manual_cli);
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
		msap1::WaveformCapture restarted(empty_device.string(), output);
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
