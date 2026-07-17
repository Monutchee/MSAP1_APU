#include "msap1/visualizer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace msap1 {
namespace {

constexpr double kAdcFullScaleCounts = 8388608.0;
constexpr std::array<const char *, adc_channel_count> kChannelNames = {
	"ILA", "ILB", "ILC", "ILN", "VLC", "VLB", "VLA", "VCM"
};

std::uint64_t frame_index(const AdcBatch &batch, std::size_t offset)
{
	const auto stride = batch.adc_sample_rate_hz / batch.display_rate_hz;
	return batch.first_frame_index + offset * stride;
}

double frame_time(const AdcBatch &batch, std::size_t offset)
{
	return static_cast<double>(frame_index(batch, offset)) /
	       static_cast<double>(batch.adc_sample_rate_hz);
}

std::string bar(std::int32_t sample)
{
	constexpr int half_width = 20;
	std::string result(half_width * 2 + 1, ' ');
	result[half_width] = '|';
	const double normalized = std::clamp(
		static_cast<double>(sample) / kAdcFullScaleCounts, -1.0, 1.0);
	const int extent = static_cast<int>(std::round(std::abs(normalized) *
						     half_width));
	if (normalized < 0.0) {
		for (int i = 1; i <= extent; ++i)
			result[half_width - i] = '=';
	} else {
		for (int i = 1; i <= extent; ++i)
			result[half_width + i] = '=';
	}
	return result;
}

} // namespace

OutputFormat parse_output_format(const std::string &name)
{
	if (name == "terminal")
		return OutputFormat::terminal;
	if (name == "table")
		return OutputFormat::table;
	if (name == "csv")
		return OutputFormat::csv;
	if (name == "jsonl")
		return OutputFormat::json_lines;
	throw std::invalid_argument("unknown output format '" + name + "'");
}

Visualizer::Visualizer(std::ostream &output, OutputFormat format,
		       std::vector<std::size_t> channels)
	: output_(output), format_(format), channels_(std::move(channels))
{
	if (channels_.empty()) {
		for (std::size_t channel = 0; channel < adc_channel_count;
		     ++channel)
			channels_.push_back(channel);
	}
	for (std::size_t index = 0; index < channels_.size(); ++index) {
		if (channels_[index] >= adc_channel_count ||
		    std::find(channels_.begin(), channels_.begin() + index,
			      channels_[index]) != channels_.begin() + index)
			throw std::invalid_argument("ADC channels must be unique values 0-7");
	}
}

void Visualizer::render(const AdcBatch &batch)
{
	++batches_seen_;
	frames_seen_ += batch.frames.size();
	if (format_ == OutputFormat::terminal)
		render_terminal(batch);
	else
		render_rows(batch);
}

void Visualizer::render_terminal(const AdcBatch &batch)
{
	if (batch.frames.empty())
		return;
	const auto latest_index = frame_index(batch, batch.frames.size() - 1);
	if (terminal_rendered_ && latest_index < terminal_next_frame_)
		return;
	terminal_next_frame_ = latest_index +
		std::max<std::uint32_t>(1u, batch.adc_sample_rate_hz / 10u);
	const auto &samples = batch.frames.back();
	if (terminal_rendered_)
		output_ << "\033[H";
	else
		output_ << "\033[2J\033[H";
	terminal_rendered_ = true;

	output_ << "AD7771 live samples (raw signed 24-bit counts)\n"
		<< "capture=" << batch.adc_sample_rate_hz << " frame/s  display="
		<< batch.display_rate_hz << " frame/s  frame="
		<< latest_index << "  flags=0x"
		<< std::hex << batch.capture_flags << std::dec << "\n\n";
	for (const auto channel : channels_) {
		const auto percentage = 100.0 * static_cast<double>(samples[channel]) /
			kAdcFullScaleCounts;
		output_ << "CH" << channel << ' ' << std::setw(3)
			<< kChannelNames[channel] << " [" << bar(samples[channel])
			<< "] " << std::setw(10) << samples[channel] << "  "
			<< std::fixed << std::setprecision(3) << std::setw(8)
			<< percentage << "% FS\n";
	}
	output_ << "\nCtrl-C to stop. Voltage/current units require board calibration.\n"
		<< std::flush;
}

void Visualizer::render_rows(const AdcBatch &batch)
{
	if (!header_written_) {
		if (format_ == OutputFormat::csv) {
			output_ << "frame,time_s,capture_flags";
			for (const auto channel : channels_)
				output_ << ",ch" << channel;
			output_ << '\n';
		} else if (format_ == OutputFormat::table) {
			output_ << std::setw(12) << "frame" << ' ' << std::setw(13)
				<< "time_s";
			for (const auto channel : channels_)
				output_ << ' ' << std::setw(11)
					<< ("ch" + std::to_string(channel));
			output_ << '\n';
		}
		header_written_ = true;
	}

	for (std::size_t offset = 0; offset < batch.frames.size(); ++offset) {
		const auto index = frame_index(batch, offset);
		const auto time = frame_time(batch, offset);
		const auto &samples = batch.frames[offset];
		if (format_ == OutputFormat::csv) {
			output_ << index << ',' << std::fixed << std::setprecision(9)
				<< time << ',' << batch.capture_flags;
			for (const auto channel : channels_)
				output_ << ',' << samples[channel];
			output_ << '\n';
		} else if (format_ == OutputFormat::table) {
			output_ << std::setw(12) << index << ' ' << std::fixed
				<< std::setprecision(6) << std::setw(13) << time;
			for (const auto channel : channels_)
				output_ << ' ' << std::setw(11) << samples[channel];
			output_ << '\n';
		} else {
			output_ << "{\"frame\":" << index << ",\"time_s\":"
				<< std::fixed << std::setprecision(9) << time
				<< ",\"capture_flags\":" << batch.capture_flags
				<< ",\"channel_ids\":[";
			for (std::size_t index = 0; index < channels_.size(); ++index) {
				if (index != 0)
					output_ << ',';
				output_ << channels_[index];
			}
			output_ << "],\"channels\":[";
			for (std::size_t index = 0; index < channels_.size(); ++index) {
				if (index != 0)
					output_ << ',';
				output_ << samples[channels_[index]];
			}
			output_ << "]}\n";
		}
	}
	output_.flush();
}

void Visualizer::finish()
{
	if (format_ == OutputFormat::terminal && terminal_rendered_)
		output_ << "\nStopped after " << frames_seen_ << " displayed frames in "
			<< batches_seen_ << " messages.\n";
	output_.flush();
}

} // namespace msap1
