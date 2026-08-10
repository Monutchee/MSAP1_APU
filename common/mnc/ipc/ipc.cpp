#include "mnc/ipc/ipc.hpp"

#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/system/system_error.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <future>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace mnc::ipc {
namespace {

using boost::asio::use_awaitable;

template<typename T>
void append_little(std::vector<std::byte> &output, T value)
{
	static_assert(std::is_unsigned_v<T>);
	for (std::size_t index = 0; index < sizeof(T); ++index)
		output.push_back(static_cast<std::byte>((value >> (index * 8u)) & 0xffu));
}

template<typename T>
T read_little(std::span<const std::byte> input)
{
	static_assert(std::is_unsigned_v<T>);
	if (input.size() != sizeof(T))
		throw std::invalid_argument("invalid little-endian field size");
	T value = 0;
	for (std::size_t index = 0; index < sizeof(T); ++index)
		value |= static_cast<T>(std::to_integer<std::uint8_t>(input[index]))
			 << (index * 8u);
	return value;
}

bool valid_kind(FrameKind kind)
{
	switch (kind) {
	case FrameKind::request:
	case FrameKind::response:
	case FrameKind::event:
	case FrameKind::error:
		return true;
	}
	return false;
}

} // namespace

void ByteWriter::u8(std::uint8_t value) { data_.push_back(static_cast<std::byte>(value)); }
void ByteWriter::u16(std::uint16_t value) { append_little(data_, value); }
void ByteWriter::u32(std::uint32_t value) { append_little(data_, value); }
void ByteWriter::u64(std::uint64_t value) { append_little(data_, value); }
void ByteWriter::i32(std::int32_t value) { u32(std::bit_cast<std::uint32_t>(value)); }
void ByteWriter::i64(std::int64_t value) { u64(std::bit_cast<std::uint64_t>(value)); }

void ByteWriter::bytes(std::span<const std::byte> value)
{
	data_.insert(data_.end(), value.begin(), value.end());
}

void ByteWriter::fixed_string(std::string_view value, std::size_t width)
{
	if (value.size() > width)
		throw std::invalid_argument("fixed string exceeds field width");
	data_.insert(data_.end(), reinterpret_cast<const std::byte *>(value.data()),
		     reinterpret_cast<const std::byte *>(value.data() + value.size()));
	data_.insert(data_.end(), width - value.size(), std::byte{0});
}

std::span<const std::byte> ByteReader::take(std::size_t size)
{
	if (size > remaining())
		throw std::invalid_argument("truncated IPC payload");
	const auto result = data_.subspan(offset_, size);
	offset_ += size;
	return result;
}

std::uint8_t ByteReader::u8()
{
	return std::to_integer<std::uint8_t>(take(1).front());
}
std::uint16_t ByteReader::u16() { return read_little<std::uint16_t>(take(2)); }
std::uint32_t ByteReader::u32() { return read_little<std::uint32_t>(take(4)); }
std::uint64_t ByteReader::u64() { return read_little<std::uint64_t>(take(8)); }
std::int32_t ByteReader::i32() { return std::bit_cast<std::int32_t>(u32()); }
std::int64_t ByteReader::i64() { return std::bit_cast<std::int64_t>(u64()); }

std::vector<std::byte> ByteReader::bytes(std::size_t size)
{
	const auto value = take(size);
	return {value.begin(), value.end()};
}

std::string ByteReader::fixed_string(std::size_t width)
{
	const auto value = take(width);
	const auto end = std::find(value.begin(), value.end(), std::byte{0});
	return {reinterpret_cast<const char *>(value.data()),
		static_cast<std::size_t>(end - value.begin())};
}

void ByteReader::require_finished() const
{
	if (remaining() != 0)
		throw std::invalid_argument("unexpected trailing IPC payload bytes");
}

std::vector<std::byte> encode_envelope(const Frame &frame)
{
	if (!valid_kind(frame.kind))
		throw std::invalid_argument("invalid IPC frame kind");
	if (frame.payload.size() > std::numeric_limits<std::uint32_t>::max())
		throw std::length_error("IPC payload is too large");
	ByteWriter writer;
	writer.u32(envelope_magic);
	writer.u16(envelope_version);
	writer.u16(static_cast<std::uint16_t>(frame.kind));
	writer.u32(frame.message_type);
	writer.u32(static_cast<std::uint32_t>(frame.payload.size()));
	writer.u64(frame.correlation_id);
	return writer.take();
}

Frame decode_envelope(std::span<const std::byte> header,
		      std::vector<std::byte> payload, std::size_t max_payload)
{
	if (header.size() != envelope_size)
		throw std::invalid_argument("invalid IPC envelope size");
	ByteReader reader(header);
	if (reader.u32() != envelope_magic)
		throw std::invalid_argument("invalid IPC envelope magic");
	if (reader.u16() != envelope_version)
		throw std::invalid_argument("unsupported IPC envelope version");
	const auto kind = static_cast<FrameKind>(reader.u16());
	if (!valid_kind(kind))
		throw std::invalid_argument("invalid IPC frame kind");
	const auto message_type = reader.u32();
	const auto payload_size = reader.u32();
	const auto correlation = reader.u64();
	reader.require_finished();
	if (payload_size > max_payload || payload_size != payload.size())
		throw std::invalid_argument("invalid IPC payload size");
	return {kind, message_type, correlation, std::move(payload)};
}

struct FramedConnection::PendingWrite {
	std::vector<std::byte> bytes;
	std::function<void(boost::system::error_code)> done;
};

FramedConnection::FramedConnection(Socket socket, ConnectionLimits limits)
	: socket_(std::move(socket)), strand_(boost::asio::make_strand(socket_.get_executor())),
	  limits_(limits)
{
}

boost::asio::awaitable<Frame> FramedConnection::receive()
{
	std::array<std::byte, envelope_size> header{};
	co_await boost::asio::async_read(socket_, boost::asio::buffer(header), use_awaitable);
	ByteReader reader(header);
	if (reader.u32() != envelope_magic)
		throw std::invalid_argument("invalid IPC envelope magic");
	if (reader.u16() != envelope_version)
		throw std::invalid_argument("unsupported IPC envelope version");
	const auto kind = static_cast<FrameKind>(reader.u16());
	if (!valid_kind(kind))
		throw std::invalid_argument("invalid IPC frame kind");
	const auto message_type = reader.u32();
	const auto payload_size = reader.u32();
	const auto correlation = reader.u64();
	if (payload_size > limits_.max_payload)
		throw std::length_error("IPC payload exceeds connection limit");
	std::vector<std::byte> payload(payload_size);
	if (!payload.empty())
		co_await boost::asio::async_read(socket_, boost::asio::buffer(payload),
					     use_awaitable);
	co_return Frame{kind, message_type, correlation, std::move(payload)};
}

void FramedConnection::enqueue(
	Frame frame, std::function<void(boost::system::error_code)> done)
{
	auto header = encode_envelope(frame);
	header.insert(header.end(), frame.payload.begin(), frame.payload.end());
	auto pending = std::make_shared<PendingWrite>();
	pending->bytes = std::move(header);
	pending->done = std::move(done);
	boost::asio::post(strand_, [self = shared_from_this(), pending]() {
		if (!self->socket_.is_open()) {
			pending->done(boost::asio::error::not_connected);
			return;
		}
		if (self->writes_.size() >= self->limits_.max_queued_frames ||
		    self->queued_bytes_ > self->limits_.max_queued_bytes ||
		    pending->bytes.size() > self->limits_.max_queued_bytes -
						 self->queued_bytes_) {
			pending->done(boost::asio::error::no_buffer_space);
			self->close();
			return;
		}
		self->queued_bytes_ += pending->bytes.size();
		self->writes_.push_back(pending);
		self->start_next_write();
	});
}

void FramedConnection::start_next_write()
{
	if (writing_ || writes_.empty())
		return;
	writing_ = true;
	auto pending = writes_.front();
	boost::asio::async_write(
		socket_, boost::asio::buffer(pending->bytes),
		boost::asio::bind_executor(
			strand_, [self = shared_from_this(), pending](
				 boost::system::error_code error, std::size_t) {
				self->queued_bytes_ -= pending->bytes.size();
				self->writes_.erase(self->writes_.begin());
				self->writing_ = false;
				pending->done(error);
				if (error) {
					self->close();
					for (auto &queued : self->writes_)
						queued->done(error);
					self->writes_.clear();
					self->queued_bytes_ = 0;
					return;
				}
				self->start_next_write();
			}));
}

boost::asio::awaitable<void> FramedConnection::send(Frame frame)
{
	auto executor = co_await boost::asio::this_coro::executor;
	auto timer = std::make_shared<boost::asio::steady_timer>(executor);
	timer->expires_at(std::chrono::steady_clock::time_point::max());
	auto result = std::make_shared<boost::system::error_code>();
	enqueue(std::move(frame), [timer, result](boost::system::error_code error) {
		*result = error;
		boost::system::error_code ignored;
		timer->cancel(ignored);
	});
	boost::system::error_code wait_error;
	co_await timer->async_wait(boost::asio::redirect_error(use_awaitable, wait_error));
	if (*result)
		throw boost::system::system_error(*result);
}

void FramedConnection::post_send(Frame frame)
{
	enqueue(std::move(frame), [](boost::system::error_code) {});
}

void FramedConnection::close() noexcept
{
	boost::system::error_code ignored;
	socket_.cancel(ignored);
	socket_.close(ignored);
}

bool FramedConnection::is_open() const noexcept { return socket_.is_open(); }

PeerCredentials FramedConnection::peer_credentials()
{
	struct ucred credentials {};
	socklen_t length = sizeof(credentials);
	if (::getsockopt(socket_.native_handle(), SOL_SOCKET, SO_PEERCRED,
			 &credentials, &length) != 0)
		throw std::runtime_error("SO_PEERCRED failed");
	return {credentials.pid, credentials.uid, credentials.gid};
}

boost::asio::any_io_executor FramedConnection::executor()
{
	return socket_.get_executor();
}

UnixStreamServer::UnixStreamServer(boost::asio::any_io_executor executor,
				   std::string path, ConnectionLimits limits)
	: acceptor_(executor), path_(std::move(path)), limits_(limits)
{
}

UnixStreamServer::~UnixStreamServer() { stop(); }

void UnixStreamServer::start(FrameHandler handler, ErrorHandler error_handler)
{
	if (started_)
		throw std::logic_error("IPC server is already started");
	const auto parent = std::filesystem::path(path_).parent_path();
	if (!parent.empty())
		std::filesystem::create_directories(parent);
	std::filesystem::remove(path_);
	handler_ = std::move(handler);
	error_handler_ = std::move(error_handler);
	boost::asio::local::stream_protocol::endpoint endpoint(path_);
	acceptor_.open(endpoint.protocol());
	acceptor_.bind(endpoint);
	acceptor_.listen();
	::chmod(path_.c_str(), 0660);
	started_ = true;
	boost::asio::co_spawn(acceptor_.get_executor(), accept_loop(), boost::asio::detached);
}

void UnixStreamServer::stop() noexcept
{
	if (!started_)
		return;
	started_ = false;
	boost::system::error_code asio_ignored;
	acceptor_.cancel(asio_ignored);
	acceptor_.close(asio_ignored);
	std::error_code filesystem_ignored;
	std::filesystem::remove(path_, filesystem_ignored);
}

boost::asio::awaitable<void> UnixStreamServer::accept_loop()
{
	while (started_) {
		try {
			auto socket = co_await acceptor_.async_accept(use_awaitable);
			auto connection =
				std::make_shared<FramedConnection>(std::move(socket), limits_);
			boost::asio::co_spawn(acceptor_.get_executor(),
					      connection_loop(std::move(connection)),
					      boost::asio::detached);
		} catch (const std::exception &error) {
			if (started_ && error_handler_)
				error_handler_(error.what());
		}
	}
}

boost::asio::awaitable<void>
UnixStreamServer::connection_loop(Connection connection)
{
	try {
		while (connection->is_open()) {
			auto frame = co_await connection->receive();
			if (handler_)
				handler_(connection, std::move(frame));
		}
	} catch (const boost::system::system_error &error) {
		/* A framed stream is normally closed by the client after its final
		 * response.  Boost reports that orderly shutdown as EOF; it is a
		 * connection lifecycle event, not a service fault.  Cancellation is
		 * likewise expected while the server is stopping. */
		if (error.code() != boost::asio::error::eof &&
		    error.code() != boost::asio::error::operation_aborted &&
		    error_handler_)
			error_handler_(error.what());
	} catch (const std::exception &error) {
		if (error_handler_)
			error_handler_(error.what());
	}
	connection->close();
}

UnixStreamClient::UnixStreamClient(boost::asio::any_io_executor executor,
				   std::string path, ConnectionLimits limits)
	: executor_(std::move(executor)), path_(std::move(path)), limits_(limits)
{
}

boost::asio::awaitable<std::shared_ptr<FramedConnection>>
UnixStreamClient::connect()
{
	FramedConnection::Socket socket(executor_);
	co_await socket.async_connect(
		boost::asio::local::stream_protocol::endpoint(path_), use_awaitable);
	co_return std::make_shared<FramedConnection>(std::move(socket), limits_);
}

struct RequestClient::PendingRequest {
	explicit PendingRequest(boost::asio::any_io_executor executor)
		: wakeup(std::move(executor))
	{
		wakeup.expires_at(std::chrono::steady_clock::time_point::max());
	}

	boost::asio::steady_timer wakeup;
	std::optional<Frame> response;
	std::exception_ptr failure;
};

RequestClient::RequestClient(boost::asio::any_io_executor executor,
			     std::string path, ConnectionLimits limits)
	: executor_(std::move(executor)), strand_(boost::asio::make_strand(executor_)),
	  path_(std::move(path)), limits_(limits)
{
}

boost::asio::awaitable<void> RequestClient::connect()
{
	co_await boost::asio::dispatch(strand_, use_awaitable);
	if (connection_ && connection_->is_open())
		co_return;
	UnixStreamClient client(strand_, path_, limits_);
	auto connection = co_await client.connect();
	connection_ = connection;
	open_ = true;
	boost::asio::co_spawn(strand_, read_loop(std::move(connection)),
		boost::asio::detached);
}

boost::asio::awaitable<Frame>
RequestClient::request(Frame frame, std::chrono::milliseconds timeout)
{
	co_await boost::asio::dispatch(strand_, use_awaitable);
	if (!connection_ || !connection_->is_open())
		throw std::runtime_error("IPC client is not connected");
	if (frame.kind != FrameKind::request)
		throw std::invalid_argument("request client requires a request frame");
	if (frame.correlation_id == 0) {
		do {
			frame.correlation_id = next_correlation_id_++;
		} while (frame.correlation_id == 0 ||
			 pending_.contains(frame.correlation_id));
	}
	if (pending_.contains(frame.correlation_id))
		throw std::invalid_argument("duplicate IPC correlation ID");

	const auto correlation = frame.correlation_id;
	auto pending = std::make_shared<PendingRequest>(strand_);
	pending_[correlation] = pending;
	try {
		co_await connection_->send(std::move(frame));
	} catch (...) {
		pending_.erase(correlation);
		throw;
	}

	/* The read coroutine may receive a very fast response while send() is
	 * waiting for the write queue to complete. In that case its timer cancel
	 * happens before this coroutine has installed a wait, so consume the
	 * already-delivered result instead of rearming a timer and timing out. */
	boost::system::error_code wait_error = boost::asio::error::operation_aborted;
	if (!pending->response && !pending->failure) {
		pending->wakeup.expires_after(timeout);
		co_await pending->wakeup.async_wait(
			boost::asio::redirect_error(use_awaitable, wait_error));
	}
	pending_.erase(correlation);
	if (!wait_error)
		throw std::runtime_error("IPC request timed out");
	if (pending->failure)
		std::rethrow_exception(pending->failure);
	if (!pending->response)
		throw std::runtime_error("IPC request ended without a response");
	co_return std::move(*pending->response);
}

void RequestClient::set_event_handler(EventHandler handler)
{
	boost::asio::post(strand_,
		[self = shared_from_this(), handler = std::move(handler)]() mutable {
			self->event_handler_ = std::move(handler);
		});
}

void RequestClient::close() noexcept
{
	/* Invalidate the public state immediately.  The actual socket close is
	 * serialized on the strand, but a caller that catches an IPC failure must
	 * never start another request on the stale connection in the meantime. */
	open_ = false;
	boost::asio::post(strand_, [self = shared_from_this()] {
		if (self->connection_)
			self->connection_->close();
		self->fail_pending(std::make_exception_ptr(
			std::runtime_error("IPC connection closed")));
	});
}

bool RequestClient::is_open() const noexcept
{
	return open_.load();
}

boost::asio::awaitable<void> RequestClient::read_loop(
	std::shared_ptr<FramedConnection> connection)
{
	try {
		while (connection->is_open()) {
			auto frame = co_await connection->receive();
			if (frame.kind == FrameKind::event) {
				if (event_handler_)
					event_handler_(std::move(frame));
				continue;
			}
			if (frame.kind != FrameKind::response &&
			    frame.kind != FrameKind::error)
				throw std::runtime_error("unexpected IPC frame kind");
			const auto found = pending_.find(frame.correlation_id);
			if (found == pending_.end())
				continue;
			found->second->response = std::move(frame);
			boost::system::error_code ignored;
			found->second->wakeup.cancel(ignored);
		}
	} catch (...) {
		/* A superseded read loop belongs to its captured connection and must
		 * not fail requests issued on a newer connection. */
		if (connection_ == connection)
			fail_pending(std::current_exception());
	}
	connection->close();
	if (connection_ == connection) {
		connection_.reset();
		open_ = false;
	}
}

void RequestClient::fail_pending(std::exception_ptr failure)
{
	for (auto &[correlation, pending] : pending_) {
		(void)correlation;
		pending->failure = failure;
		boost::system::error_code ignored;
		pending->wakeup.cancel(ignored);
	}
}

BlockingClient::BlockingClient(std::string path, ConnectionLimits limits)
	: path_(std::move(path)), limits_(limits)
{
}

Frame BlockingClient::request(Frame request, int timeout_ms) const
{
	boost::asio::io_context context;
	std::optional<Frame> response;
	std::exception_ptr failure;
	auto connection = std::make_shared<std::shared_ptr<FramedConnection>>();
	boost::asio::steady_timer deadline(context);
	deadline.expires_after(std::chrono::milliseconds(timeout_ms));
	deadline.async_wait([connection](boost::system::error_code error) {
		if (!error && *connection)
			(*connection)->close();
	});
	boost::asio::co_spawn(
		context,
		[&]() -> boost::asio::awaitable<void> {
			try {
				UnixStreamClient client(co_await boost::asio::this_coro::executor,
						       path_, limits_);
				*connection = co_await client.connect();
				co_await (*connection)->send(std::move(request));
				response = co_await (*connection)->receive();
				boost::system::error_code ignored;
				deadline.cancel(ignored);
			} catch (...) {
				failure = std::current_exception();
			}
		},
		boost::asio::detached);
	context.run();
	if (failure)
		std::rethrow_exception(failure);
	if (!response)
		throw std::runtime_error("IPC request timed out");
	return std::move(*response);
}

struct PersistentBlockingClient::Impl {
	Impl(std::string path, ConnectionLimits limits)
		: work(boost::asio::make_work_guard(context)),
		  client(std::make_shared<RequestClient>(
			  context.get_executor(), std::move(path), limits)),
		  worker([this] { context.run(); })
	{
	}

	~Impl()
	{
		client->close();
		work.reset();
		context.stop();
		if (worker.joinable())
			worker.join();
	}

	boost::asio::io_context context;
	boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work;
	std::shared_ptr<RequestClient> client;
	std::mutex connect_mutex;
	std::thread worker;
};

PersistentBlockingClient::PersistentBlockingClient(
	std::string path, ConnectionLimits limits)
	: impl_(std::make_unique<Impl>(std::move(path), limits))
{
}

PersistentBlockingClient::~PersistentBlockingClient() = default;
PersistentBlockingClient::PersistentBlockingClient(
	PersistentBlockingClient &&) noexcept = default;
PersistentBlockingClient &PersistentBlockingClient::operator=(
	PersistentBlockingClient &&) noexcept = default;

Frame PersistentBlockingClient::request(Frame frame, int timeout_ms) const
{
	if (!impl_)
		throw std::runtime_error("persistent IPC client has been moved from");
	try {
		if (!impl_->client->is_open()) {
			std::scoped_lock connect_lock(impl_->connect_mutex);
			if (!impl_->client->is_open())
				boost::asio::co_spawn(impl_->context,
					impl_->client->connect(), boost::asio::use_future)
					.get();
		}
		return boost::asio::co_spawn(
			impl_->context,
			impl_->client->request(
				std::move(frame), std::chrono::milliseconds(timeout_ms)),
			boost::asio::use_future)
			.get();
	} catch (...) {
		/* Never retry a side-effecting frame whose response was lost. Closing
		 * makes the next independent product request reconnect cleanly. */
		impl_->client->close();
		throw;
	}
}

void PersistentBlockingClient::close() noexcept
{
	if (impl_)
		impl_->client->close();
}

bool PersistentBlockingClient::is_open() const noexcept
{
	return impl_ && impl_->client->is_open();
}

} // namespace mnc::ipc
