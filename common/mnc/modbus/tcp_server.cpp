#include "mnc/modbus/tcp_server.hpp"

#include "mnc/modbus/encoding.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/system_error.hpp>

#include <array>
#include <atomic>
#include <mutex>
#include <set>
#include <stdexcept>
#include <utility>

namespace mnc::modbus {
namespace {

using boost::asio::awaitable;
using boost::asio::use_awaitable;

constexpr std::size_t tcp_header_size = 7;
constexpr std::size_t maximum_pdu_size = 253;

std::uint8_t octet(std::byte value)
{
	return std::to_integer<std::uint8_t>(value);
}

void report(const ErrorHandler &handler, std::string_view transport,
	std::string message)
{
	if (handler)
		handler(transport, message);
}

bool expected_disconnect(const boost::system::error_code &error)
{
	return error == boost::asio::error::eof ||
	       error == boost::asio::error::connection_reset ||
	       error == boost::asio::error::operation_aborted ||
	       error == boost::asio::error::broken_pipe ||
	       error == boost::asio::error::not_connected;
}

} // namespace

struct ModbusTcpServer::Impl : std::enable_shared_from_this<Impl> {
	using Socket = boost::asio::ip::tcp::socket;
	using SocketPtr = std::shared_ptr<Socket>;

	Impl(boost::asio::any_io_executor executor, RequestHandler &handler,
	     TcpServerConfig config, ErrorHandler errors)
		: executor(std::move(executor)), acceptor(this->executor),
		  handler(handler), config(std::move(config)),
		  errors(std::move(errors))
	{
	}

	void release(const SocketPtr &socket) noexcept
	{
		boost::system::error_code ignored;
		/* release() runs on an I/O worker while stop() may run on the
		 * lifecycle thread. Serialize ownership removal and close so the
		 * same Boost.Asio socket is never closed concurrently. */
		std::lock_guard lock(sockets_mutex);
		sockets.erase(socket);
		socket->cancel(ignored);
		socket->close(ignored);
	}

	[[nodiscard]] bool current(std::uint64_t run) const noexcept
	{
		return running.load() && generation.load() == run;
	}

	awaitable<void> session(SocketPtr socket, std::uint64_t run)
	{
		struct Guard {
			Impl &owner;
			SocketPtr socket;
			~Guard() { owner.release(socket); }
		} guard{*this, socket};

		try {
			while (current(run)) {
				std::array<std::byte, tcp_header_size> header{};
				co_await boost::asio::async_read(*socket,
					boost::asio::buffer(header), use_awaitable);
				const auto transaction = read_u16_be(header);
				const auto protocol = read_u16_be(
					std::span(header).subspan(2, 2));
				const auto length = read_u16_be(
					std::span(header).subspan(4, 2));
				if (protocol != 0 || length < 2 ||
				    length > maximum_pdu_size + 1)
					throw std::runtime_error("invalid MBAP header");
				const auto unit = octet(header[6]);
				std::vector<std::byte> pdu(length - 1);
				co_await boost::asio::async_read(*socket,
					boost::asio::buffer(pdu), use_awaitable);
				auto response = handler.handle(
					Request{unit, pdu, false}, config.unit_id);
				if (!response)
					continue;
				std::vector<std::byte> adu;
				adu.reserve(tcp_header_size + response->pdu.size());
				append_u16_be(adu, transaction);
				append_u16_be(adu, 0);
				append_u16_be(adu, static_cast<std::uint16_t>(
					response->pdu.size() + 1));
				adu.push_back(static_cast<std::byte>(response->unit_id));
				adu.insert(adu.end(), response->pdu.begin(),
					response->pdu.end());
				co_await boost::asio::async_write(*socket,
					boost::asio::buffer(adu), use_awaitable);
			}
		} catch (const boost::system::system_error &error) {
			if (current(run) && !expected_disconnect(error.code()))
				report(errors, "tcp", error.what());
		} catch (const std::exception &error) {
			if (current(run))
				report(errors, "tcp", error.what());
		}
	}

	bool reserve(const SocketPtr &socket, std::uint64_t run)
	{
		/* Set membership is the slot count. Keeping the limit check and
		 * insertion under one lock makes admission atomic and cannot leak a
		 * separate counter if set::insert throws. */
		std::lock_guard lock(sockets_mutex);
		if (!current(run) || sockets.size() >= config.maximum_clients)
			return false;
		sockets.insert(socket);
		return true;
	}

	awaitable<void> accept_loop(std::uint64_t run)
	{
		while (current(run)) {
			try {
				auto socket = std::make_shared<Socket>(executor);
				co_await acceptor.async_accept(*socket, use_awaitable);
				/* A canceled accept from an earlier stop() may complete after
				 * start() opens a new run. The generation prevents that stale
				 * coroutine from reserving a slot or spawning a session. */
				if (!reserve(socket, run)) {
					boost::system::error_code ignored;
					socket->close(ignored);
					if (!current(run))
						co_return;
					continue;
				}

				auto self = shared_from_this();
				try {
					boost::asio::co_spawn(executor,
						[self, socket, run]() -> awaitable<void> {
							co_await self->session(socket, run);
						}, boost::asio::detached);
				} catch (...) {
					release(socket);
					throw;
				}
			} catch (const boost::system::system_error &error) {
				if (current(run) && !expected_disconnect(error.code()))
					report(errors, "tcp", error.what());
			} catch (const std::exception &error) {
				if (current(run))
					report(errors, "tcp", error.what());
			}
		}
	}

	boost::asio::any_io_executor executor;
	boost::asio::ip::tcp::acceptor acceptor;
	RequestHandler &handler;
	TcpServerConfig config;
	ErrorHandler errors;
	std::atomic<bool> running{false};
	std::atomic<std::uint64_t> generation{0};
	std::mutex sockets_mutex;
	std::set<SocketPtr, std::owner_less<SocketPtr>> sockets;
};

ModbusTcpServer::ModbusTcpServer(boost::asio::any_io_executor executor,
	RequestHandler &handler, TcpServerConfig config, ErrorHandler errors)
	: impl_(std::make_shared<Impl>(std::move(executor), handler,
		std::move(config), std::move(errors)))
{
}

ModbusTcpServer::~ModbusTcpServer()
{
	stop();
}

void ModbusTcpServer::start()
{
	if (impl_->running.exchange(true))
		return;
	const auto run = impl_->generation.fetch_add(1) + 1;
	try {
		if (impl_->config.unit_id == 0 || impl_->config.unit_id > 247)
			throw std::invalid_argument(
				"Modbus TCP unit id must be 1..247");
		if (impl_->config.maximum_clients == 0)
			throw std::invalid_argument(
				"Modbus TCP maximum clients must be nonzero");
		const auto address = boost::asio::ip::make_address(
			impl_->config.bind_address);
		const boost::asio::ip::tcp::endpoint endpoint(address,
			impl_->config.port);
		impl_->acceptor.open(endpoint.protocol());
		impl_->acceptor.set_option(
			boost::asio::socket_base::reuse_address(true));
		impl_->acceptor.bind(endpoint);
		impl_->acceptor.listen();
		auto impl = impl_;
		boost::asio::co_spawn(impl_->executor,
			[impl, run]() -> awaitable<void> {
				co_await impl->accept_loop(run);
			}, boost::asio::detached);
	} catch (...) {
		impl_->running = false;
		boost::system::error_code ignored;
		impl_->acceptor.close(ignored);
		throw;
	}
}

void ModbusTcpServer::stop() noexcept
{
	if (!impl_ || !impl_->running.exchange(false))
		return;
	++impl_->generation;
	boost::system::error_code ignored;
	impl_->acceptor.cancel(ignored);
	impl_->acceptor.close(ignored);
	std::lock_guard lock(impl_->sockets_mutex);
	for (const auto &socket : impl_->sockets) {
		socket->cancel(ignored);
		socket->close(ignored);
	}
	/* Session coroutines retain their shared_ptr. Removing stopped-run
	 * sockets now lets a new run admit clients without waiting for canceled
	 * reads to unwind. */
	impl_->sockets.clear();
}

std::uint16_t ModbusTcpServer::local_port() const
{
	return impl_->acceptor.local_endpoint().port();
}

} // namespace mnc::modbus
