#ifndef MSAP1_ACQUISITION_IPC_HPP
#define MSAP1_ACQUISITION_IPC_HPP

/**
 * Acquisition IPC transport binding.
 *
 * `mnc::ipc` owns stream framing; `acquisition_commands.hpp` owns the typed
 * command vocabulary.  This header binds the two: BEVE payload encoding, the
 * blocking product client, and the daemon-side command registry.
 */

#include "msap1/acquisition_commands.hpp"
#include "mnc/ipc/ipc.hpp"

#include <glaze/glaze.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace msap1 {

class AcquisitionUnavailable : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

template <typename Request>
[[nodiscard]] mnc::ipc::Frame encode_acquisition_request(const Request &request)
{
	auto buffer = glz::write_beve(request);
	if (!buffer)
		throw std::invalid_argument(
			"failed to encode acquisition request");
	return {mnc::ipc::FrameKind::request, acquisition_command_id<Request>,
		0, mnc::ipc::to_payload(*buffer)};
}

template <typename Response>
[[nodiscard]] mnc::ipc::Frame
encode_acquisition_response(const Response &response,
			    std::uint32_t message_type,
			    std::uint64_t correlation_id)
{
	auto buffer = glz::write_beve(response);
	if (!buffer)
		throw std::invalid_argument(
			"failed to encode acquisition response");
	return {response.status == AcquisitionStatus::ok
			? mnc::ipc::FrameKind::response
			: mnc::ipc::FrameKind::error,
		message_type, correlation_id, mnc::ipc::to_payload(*buffer)};
}

template <typename Message>
[[nodiscard]] Message decode_acquisition_payload(const mnc::ipc::Frame &frame)
{
	Message message{};
	if (glz::read_beve(message, mnc::ipc::payload_view(frame.payload)))
		throw std::invalid_argument(
			"malformed acquisition IPC payload");
	return message;
}

/**
 * Blocking acquisition client over one persistent correlation-aware stream.
 *
 * Callers hand in a request struct and receive its paired response struct;
 * transport failures surface as AcquisitionUnavailable.  Response status is
 * product data and stays the caller's concern.
 */
class AcquisitionClient {
public:
	explicit AcquisitionClient(
		std::string socket_path = acquisition_socket_path);
	~AcquisitionClient();
	AcquisitionClient(const AcquisitionClient &) = delete;
	AcquisitionClient &operator=(const AcquisitionClient &) = delete;
	AcquisitionClient(AcquisitionClient &&) noexcept;
	AcquisitionClient &operator=(AcquisitionClient &&) noexcept;

	template <typename Request>
	typename Request::Response request(const Request &message,
					   int timeout_ms = 3000)
	{
		const auto reply = transport(
			encode_acquisition_request(message), timeout_ms);
		if (reply.message_type != acquisition_command_id<Request>)
			throw AcquisitionUnavailable(
				"invalid acquisition response identity");
		try {
			return decode_acquisition_payload<
				typename Request::Response>(reply);
		} catch (const std::exception &error) {
			throw AcquisitionUnavailable(error.what());
		}
	}

private:
	/** Sends one frame and waits for its correlated reply. */
	[[nodiscard]] mnc::ipc::Frame transport(mnc::ipc::Frame frame,
						int timeout_ms);

	struct Impl;
	std::unique_ptr<Impl> impl_;
};

/**
 * Daemon-side command table: message_type -> decode, handle, encode.
 *
 * A handler receives the decoded request struct and returns its response
 * struct.  Exceptions become an error response carrying `failure_status`;
 * malformed or version-mismatched requests become bad_request.
 */
class AcquisitionCommandRegistry {
public:
	using ErrorObserver = std::function<void(std::string_view command,
						 std::string_view what)>;

	void set_error_observer(ErrorObserver observer)
	{
		observer_ = std::move(observer);
	}

	template <typename Request, typename Handler>
	void on(AcquisitionStatus failure_status, Handler handler)
	{
		handlers_[acquisition_command_id<Request>] =
			[this, failure_status, handler = std::move(handler)](
				const mnc::ipc::Frame &frame) {
				Request request{};
				try {
					request = decode_acquisition_payload<
						Request>(frame);
					if (request.version !=
					    acquisition_ipc_version)
						throw std::invalid_argument(
							"unsupported acquisition IPC version");
				} catch (const std::exception &error) {
					return reject(Request::command,
						      error.what(),
						      AcquisitionStatus::bad_request,
						      frame);
				}
				try {
					return encode_acquisition_response(
						handler(request),
						frame.message_type,
						frame.correlation_id);
				} catch (const std::exception &error) {
					return reject(Request::command,
						      error.what(),
						      failure_status, frame);
				}
			};
	}

	[[nodiscard]] mnc::ipc::Frame dispatch(const mnc::ipc::Frame &frame) const
	{
		const auto handler = handlers_.find(frame.message_type);
		if (handler == handlers_.end() ||
		    frame.kind != mnc::ipc::FrameKind::request)
			return reject("unknown", "unknown acquisition command",
				      AcquisitionStatus::bad_request, frame);
		return handler->second(frame);
	}

private:
	/** Status-only payload; decodes into any response type (fields keep
	 * their defaults), so rejections never need the command's schema. */
	struct StatusOnlyResponse {
		AcquisitionStatus status = AcquisitionStatus::ok;
	};

	[[nodiscard]] mnc::ipc::Frame reject(std::string_view command,
					     std::string_view what,
					     AcquisitionStatus status,
					     const mnc::ipc::Frame &frame) const
	{
		if (observer_)
			observer_(command, what);
		return encode_acquisition_response(StatusOnlyResponse{status},
						   frame.message_type,
						   frame.correlation_id);
	}

	std::unordered_map<std::uint32_t,
			   std::function<mnc::ipc::Frame(const mnc::ipc::Frame &)>>
		handlers_;
	ErrorObserver observer_;
};

} // namespace msap1

#endif
