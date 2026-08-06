#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/strand.hpp>
#include <boost/system/error_code.hpp>

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <atomic>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mnc::ipc {

inline constexpr std::uint32_t envelope_magic = 0x49434e4du; // "MNCI"
inline constexpr std::uint16_t envelope_version = 1;
inline constexpr std::size_t envelope_size = 24;
inline constexpr std::size_t default_max_payload = 1024u * 1024u;

enum class FrameKind : std::uint16_t {
	request = 1,
	response = 2,
	event = 3,
	error = 4,
};

struct Frame {
	FrameKind kind = FrameKind::request;
	std::uint32_t message_type = 0;
	std::uint64_t correlation_id = 0;
	std::vector<std::byte> payload;
};

/** Adapts text-oriented serializers (JSON/BEVE buffers) to Frame::payload. */
[[nodiscard]] inline std::vector<std::byte> to_payload(std::string_view text)
{
	const auto *data = reinterpret_cast<const std::byte *>(text.data());
	return {data, data + text.size()};
}

[[nodiscard]] inline std::string_view
payload_view(std::span<const std::byte> payload)
{
	return {reinterpret_cast<const char *>(payload.data()), payload.size()};
}

struct PeerCredentials {
	std::int32_t pid = -1;
	std::uint32_t uid = 0;
	std::uint32_t gid = 0;
};

class ByteWriter {
public:
	void u8(std::uint8_t value);
	void u16(std::uint16_t value);
	void u32(std::uint32_t value);
	void u64(std::uint64_t value);
	void i32(std::int32_t value);
	void i64(std::int64_t value);
	void bytes(std::span<const std::byte> value);
	void fixed_string(std::string_view value, std::size_t width);

	[[nodiscard]] const std::vector<std::byte> &data() const noexcept
	{
		return data_;
	}
	[[nodiscard]] std::vector<std::byte> take() noexcept
	{
		return std::move(data_);
	}

private:
	std::vector<std::byte> data_;
};

class ByteReader {
public:
	explicit ByteReader(std::span<const std::byte> data) : data_(data) {}

	std::uint8_t u8();
	std::uint16_t u16();
	std::uint32_t u32();
	std::uint64_t u64();
	std::int32_t i32();
	std::int64_t i64();
	std::vector<std::byte> bytes(std::size_t size);
	std::string fixed_string(std::size_t width);
	void require_finished() const;

	[[nodiscard]] std::size_t remaining() const noexcept
	{
		return data_.size() - offset_;
	}

private:
	std::span<const std::byte> take(std::size_t size);
	std::span<const std::byte> data_;
	std::size_t offset_ = 0;
};

struct ConnectionLimits {
	std::size_t max_payload = default_max_payload;
	std::size_t max_queued_frames = 128;
	std::size_t max_queued_bytes = 2u * default_max_payload;
};

class FramedConnection : public std::enable_shared_from_this<FramedConnection> {
public:
	using Socket = boost::asio::local::stream_protocol::socket;

	explicit FramedConnection(Socket socket, ConnectionLimits limits = {});

	boost::asio::awaitable<Frame> receive();
	boost::asio::awaitable<void> send(Frame frame);
	void post_send(Frame frame);
	void close() noexcept;

	[[nodiscard]] bool is_open() const noexcept;
	[[nodiscard]] PeerCredentials peer_credentials();
	[[nodiscard]] boost::asio::any_io_executor executor();

private:
	struct PendingWrite;
	void enqueue(Frame frame, std::function<void(boost::system::error_code)> done);
	void start_next_write();

	Socket socket_;
	boost::asio::strand<boost::asio::any_io_executor> strand_;
	ConnectionLimits limits_;
	std::vector<std::shared_ptr<PendingWrite>> writes_;
	std::size_t queued_bytes_ = 0;
	bool writing_ = false;
};

class UnixStreamServer {
public:
	using Connection = std::shared_ptr<FramedConnection>;
	using FrameHandler = std::function<void(Connection, Frame)>;
	using ErrorHandler = std::function<void(const std::string &)>;

	UnixStreamServer(boost::asio::any_io_executor executor,
			 std::string path, ConnectionLimits limits = {});
	~UnixStreamServer();

	UnixStreamServer(const UnixStreamServer &) = delete;
	UnixStreamServer &operator=(const UnixStreamServer &) = delete;

	void start(FrameHandler handler, ErrorHandler error_handler = {});
	void stop() noexcept;

	[[nodiscard]] const std::string &path() const noexcept { return path_; }

private:
	boost::asio::awaitable<void> accept_loop();
	boost::asio::awaitable<void> connection_loop(Connection connection);

	boost::asio::local::stream_protocol::acceptor acceptor_;
	std::string path_;
	ConnectionLimits limits_;
	FrameHandler handler_;
	ErrorHandler error_handler_;
	bool started_ = false;
};

class UnixStreamClient {
public:
	UnixStreamClient(boost::asio::any_io_executor executor,
			 std::string path, ConnectionLimits limits = {});

	boost::asio::awaitable<std::shared_ptr<FramedConnection>> connect();

private:
	boost::asio::any_io_executor executor_;
	std::string path_;
	ConnectionLimits limits_;
};

/**
 * Persistent correlation-aware request client.
 *
 * The product layer owns the executor and decides when to connect or
 * reconnect.  A single reader coroutine routes responses to independent
 * request waiters and forwards server-pushed event frames to the configured
 * callback.
 */
class RequestClient : public std::enable_shared_from_this<RequestClient> {
public:
	using EventHandler = std::function<void(Frame)>;

	RequestClient(boost::asio::any_io_executor executor, std::string path,
		      ConnectionLimits limits = {});

	boost::asio::awaitable<void> connect();
	boost::asio::awaitable<Frame>
	request(Frame request, std::chrono::milliseconds timeout);
	void set_event_handler(EventHandler handler);
	void close() noexcept;

	[[nodiscard]] bool is_open() const noexcept;

private:
	struct PendingRequest;
	boost::asio::awaitable<void>
	read_loop(std::shared_ptr<FramedConnection> connection);
	void fail_pending(std::exception_ptr failure);

	boost::asio::any_io_executor executor_;
	boost::asio::strand<boost::asio::any_io_executor> strand_;
	std::string path_;
	ConnectionLimits limits_;
	std::shared_ptr<FramedConnection> connection_;
	std::unordered_map<std::uint64_t, std::shared_ptr<PendingRequest>> pending_;
	EventHandler event_handler_;
	std::uint64_t next_correlation_id_ = 1;
	std::atomic<bool> open_{false};
};

class BlockingClient {
public:
	explicit BlockingClient(std::string path, ConnectionLimits limits = {});
	Frame request(Frame request, int timeout_ms = 3000) const;

private:
	std::string path_;
	ConnectionLimits limits_;
};

[[nodiscard]] std::vector<std::byte> encode_envelope(const Frame &frame);
[[nodiscard]] Frame decode_envelope(std::span<const std::byte> header,
				    std::vector<std::byte> payload,
				    std::size_t max_payload = default_max_payload);

} // namespace mnc::ipc
