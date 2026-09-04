#pragma once

#include "msap1/waveform/waveform_conversion_ipc.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace msap1::web {

class WaveformConversionGatewayError final : public std::runtime_error {
public:
	WaveformConversionGatewayError(waveform::ipc::Status status,
		std::string message, std::string code = {},
		std::vector<std::string> missing_fields = {});
	[[nodiscard]] waveform::ipc::Status status() const noexcept;
	[[nodiscard]] const std::string &code() const noexcept { return code_; }
	[[nodiscard]] const std::vector<std::string> &missing_fields() const noexcept
	{
		return missing_fields_;
	}

private:
	waveform::ipc::Status status_;
	std::string code_;
	std::vector<std::string> missing_fields_;
};

/** Typed Web boundary for the optional waveform conversion daemon. */
class WaveformConversionGateway final {
public:
	using Capabilities = waveform::ipc::Capabilities;
	using Job = waveform::ipc::Job;
	using Chunk = waveform::ipc::Response;

	explicit WaveformConversionGateway(std::string socket =
		std::string(waveform::ipc::socket_path));

	[[nodiscard]] Capabilities capabilities(int timeout_ms = 1000) const;
	[[nodiscard]] Job submit(std::string owner, std::string session_id,
		std::string source_basename, std::string scope, std::string event_id,
		std::string format, int timeout_ms = 5000) const;
	[[nodiscard]] Job status(std::string owner, std::string job_id,
		int timeout_ms = 5000) const;
	[[nodiscard]] Job cancel(std::string owner, std::string job_id,
		int timeout_ms = 5000) const;
	[[nodiscard]] Chunk read_chunk(std::string owner, std::string job_id,
		std::uint64_t offset, std::uint32_t limit,
		int timeout_ms = 5000) const;

private:
	[[nodiscard]] waveform::ipc::Response require_ok(
		waveform::ipc::Request request, int timeout_ms) const;

	mutable waveform::ipc::WaveformConversionClient client_;
};

} // namespace msap1::web
