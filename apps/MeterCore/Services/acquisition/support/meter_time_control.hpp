#pragma once

#include "msap1/waveform/waveform_capture.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace msap1::acquisition::daemon {

struct MeterTimeSync {
	std::uint64_t sample_counter = 0;
	std::uint64_t pl_tick = 0;
	std::uint64_t tai_nanoseconds = 0;
	std::uint64_t realtime_nanoseconds = 0;
	std::uint64_t bracket_nanoseconds = 0;
};

struct Frequency10sBoundary {
	std::uint64_t start_sample_index = 0;
	std::uint64_t end_sample_index = 0;
	std::uint64_t utc_start_nanoseconds = 0;
	std::uint64_t utc_end_nanoseconds = 0;
	std::uint64_t utc_uncertainty_nanoseconds = 0;
	std::uint32_t measured_sample_rate_millihz = 0;
	std::uint32_t boundary_generation = 0;
	std::uint8_t nominal_frequency_hz = 0;
	std::uint8_t reference_channel = 0;
	std::uint8_t filter_profile = 0;
	std::uint8_t calibration_profile = 0;
	bool valid = false;
	bool time_synchronized = false;
};

struct Frequency10sObserverStatus {
	std::uint32_t status = 0;
	std::uint32_t completed_count = 0;
	std::uint32_t dropped_count = 0;
	std::uint32_t overflow_count = 0;
	std::uint32_t discontinuity_count = 0;
};

/** Exclusive userspace owner of the independent /dev/meter-time endpoint. */
class MeterTimeControl final {
public:
	explicit MeterTimeControl(std::string device_path);
	~MeterTimeControl();
	MeterTimeControl(const MeterTimeControl &) = delete;
	MeterTimeControl &operator=(const MeterTimeControl &) = delete;

	[[nodiscard]] std::optional<MeterTimeSync> time_sync() const noexcept;
	[[nodiscard]] std::optional<msap1::WaveformCorrelation>
	waveform_correlation() const noexcept;
	void program_ten_minute_boundary(std::uint64_t target_sample_index,
		bool valid);
	[[nodiscard]] Frequency10sObserverStatus program_frequency_10s_boundary(
		const Frequency10sBoundary &boundary);
	void cancel_frequency_10s_boundary();

private:
	std::string device_path_;
	int fd_ = -1;
};

} // namespace msap1::acquisition::daemon
