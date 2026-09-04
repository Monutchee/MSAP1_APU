#include "gateway/waveform_conversion_gateway.hpp"

#include <string_view>
#include <utility>

#include <glaze/glaze.hpp>

namespace msap1::web {
namespace {

template<class T>
T decode(std::string_view json, std::string_view description)
{
	T result;
	if (glz::read_json(result, json))
		throw std::runtime_error("invalid waveform converter " +
			std::string(description) + " response");
	return result;
}

} // namespace

WaveformConversionGatewayError::WaveformConversionGatewayError(
	waveform::ipc::Status status, std::string message, std::string code,
	std::vector<std::string> missing_fields)
	: std::runtime_error(message.empty()
		? "waveform converter request failed" : std::move(message)),
	  status_(status), code_(std::move(code)),
	  missing_fields_(std::move(missing_fields))
{
}

waveform::ipc::Status WaveformConversionGatewayError::status() const noexcept
{
	return status_;
}

WaveformConversionGateway::WaveformConversionGateway(std::string socket)
	: client_(std::move(socket))
{
}

waveform::ipc::Response WaveformConversionGateway::require_ok(
	waveform::ipc::Request request, int timeout_ms) const
{
	auto response = client_.request(std::move(request), timeout_ms);
	if (response.status != waveform::ipc::Status::ok) {
		waveform::ipc::ErrorDetail detail;
		if (!response.json.empty())
			(void)glz::read_json(detail, response.json);
		throw WaveformConversionGatewayError(response.status,
			std::move(response.message), std::move(detail.code),
			std::move(detail.missing_fields));
	}
	return response;
}

WaveformConversionGateway::Capabilities
WaveformConversionGateway::capabilities(int timeout_ms) const
{
	waveform::ipc::Request request;
	request.command = waveform::ipc::Command::capabilities;
	return decode<Capabilities>(require_ok(std::move(request), timeout_ms).json,
		"capabilities");
}

WaveformConversionGateway::Job WaveformConversionGateway::submit(
	std::string owner, std::string session_id, std::string source_basename,
	std::string scope, std::string event_id, std::string format,
	int timeout_ms) const
{
	waveform::ipc::Request request;
	request.command = waveform::ipc::Command::submit;
	request.owner = std::move(owner);
	request.session_id = std::move(session_id);
	request.source_basename = std::move(source_basename);
	request.scope = std::move(scope);
	request.event_id = std::move(event_id);
	request.format = std::move(format);
	return decode<Job>(require_ok(std::move(request), timeout_ms).json, "job");
}

WaveformConversionGateway::Job WaveformConversionGateway::status(
	std::string owner, std::string job_id, int timeout_ms) const
{
	waveform::ipc::Request request;
	request.command = waveform::ipc::Command::status;
	request.owner = std::move(owner);
	request.job_id = std::move(job_id);
	return decode<Job>(require_ok(std::move(request), timeout_ms).json, "job");
}

WaveformConversionGateway::Job WaveformConversionGateway::cancel(
	std::string owner, std::string job_id, int timeout_ms) const
{
	waveform::ipc::Request request;
	request.command = waveform::ipc::Command::cancel;
	request.owner = std::move(owner);
	request.job_id = std::move(job_id);
	return decode<Job>(require_ok(std::move(request), timeout_ms).json, "job");
}

WaveformConversionGateway::Chunk WaveformConversionGateway::read_chunk(
	std::string owner, std::string job_id, std::uint64_t offset,
	std::uint32_t limit, int timeout_ms) const
{
	waveform::ipc::Request request;
	request.command = waveform::ipc::Command::read_chunk;
	request.owner = std::move(owner);
	request.job_id = std::move(job_id);
	request.offset = offset;
	request.limit = limit;
	return require_ok(std::move(request), timeout_ms);
}

} // namespace msap1::web
