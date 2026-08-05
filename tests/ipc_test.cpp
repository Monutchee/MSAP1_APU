#include "mnc/ipc/ipc.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;

void require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

template<typename Function>
void require_throws(Function function, const char *message)
{
	try {
		function();
	} catch (const std::exception &) {
		return;
	}
	throw std::runtime_error(message);
}

std::vector<std::byte> wire_frame(const mnc::ipc::Frame &frame)
{
	auto bytes = mnc::ipc::encode_envelope(frame);
	bytes.insert(bytes.end(), frame.payload.begin(), frame.payload.end());
	return bytes;
}

void write_all(int descriptor, std::span<const std::byte> bytes,
	       std::size_t fragment)
{
	for (std::size_t offset = 0; offset < bytes.size();) {
		const auto count = std::min(fragment, bytes.size() - offset);
		const auto written = ::write(descriptor, bytes.data() + offset, count);
		if (written <= 0)
			throw std::runtime_error("raw Unix socket write failed");
		offset += static_cast<std::size_t>(written);
	}
}

void read_all(int descriptor, std::span<std::byte> bytes)
{
	for (std::size_t offset = 0; offset < bytes.size();) {
		const auto count = ::read(descriptor, bytes.data() + offset,
					  bytes.size() - offset);
		if (count <= 0)
			throw std::runtime_error("raw Unix socket read failed");
		offset += static_cast<std::size_t>(count);
	}
}

mnc::ipc::Frame read_raw_frame(int descriptor)
{
	std::array<std::byte, mnc::ipc::envelope_size> header{};
	read_all(descriptor, header);
	mnc::ipc::ByteReader reader(header);
	require(reader.u32() == mnc::ipc::envelope_magic,
		"server response magic was invalid");
	(void)reader.u16();
	(void)reader.u16();
	(void)reader.u32();
	const auto payload_size = reader.u32();
	(void)reader.u64();
	std::vector<std::byte> payload(payload_size);
	read_all(descriptor, payload);
	return mnc::ipc::decode_envelope(header, std::move(payload));
}

int connect_raw(const std::filesystem::path &path)
{
	const int descriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
	if (descriptor < 0)
		throw std::runtime_error("cannot create raw Unix socket");
	sockaddr_un address{};
	address.sun_family = AF_UNIX;
	if (path.string().size() >= sizeof(address.sun_path)) {
		::close(descriptor);
		throw std::runtime_error("test Unix socket path is too long");
	}
	std::strcpy(address.sun_path, path.c_str());
	if (::connect(descriptor, reinterpret_cast<sockaddr *>(&address),
		    sizeof(address)) != 0) {
		::close(descriptor);
		throw std::runtime_error("cannot connect raw Unix socket");
	}
	return descriptor;
}

void byte_codec_and_envelope()
{
	mnc::ipc::ByteWriter writer;
	writer.u8(0xa5);
	writer.u16(0x1234);
	writer.u32(0x89abcdefu);
	writer.u64(0x0123456789abcdefull);
	writer.i64(-42);
	writer.fixed_string("meter", 8);
	mnc::ipc::ByteReader reader(writer.data());
	require(reader.u8() == 0xa5 && reader.u16() == 0x1234 &&
		reader.u32() == 0x89abcdefu &&
		reader.u64() == 0x0123456789abcdefull &&
		reader.i64() == -42 && reader.fixed_string(8) == "meter",
		"little-endian byte codec round trip failed");
	reader.require_finished();

	mnc::ipc::Frame frame{mnc::ipc::FrameKind::request, 17, 99,
		{std::byte{1}, std::byte{2}, std::byte{3}}};
	const auto header = mnc::ipc::encode_envelope(frame);
	auto decoded = mnc::ipc::decode_envelope(header, frame.payload);
	require(decoded.kind == frame.kind &&
		decoded.message_type == frame.message_type &&
		decoded.correlation_id == frame.correlation_id &&
		decoded.payload == frame.payload,
		"IPC envelope round trip failed");

	auto invalid_magic = header;
	invalid_magic[0] = std::byte{0};
	require_throws(
		[&] { (void)mnc::ipc::decode_envelope(invalid_magic, frame.payload); },
		"invalid envelope magic was accepted");
	auto invalid_kind = header;
	invalid_kind[6] = std::byte{0xff};
	invalid_kind[7] = std::byte{0xff};
	require_throws(
		[&] { (void)mnc::ipc::decode_envelope(invalid_kind, frame.payload); },
		"invalid envelope flags were accepted");
	require_throws(
		[&] { (void)mnc::ipc::decode_envelope(header, {}, 2); },
		"oversized or truncated envelope payload was accepted");
}

void fragmented_and_coalesced_streams()
{
	const auto path = std::filesystem::temp_directory_path() /
		("mnc-ipc-test-" + std::to_string(::getpid()) + ".sock");
	boost::asio::io_context context;
	std::atomic<std::uint32_t> requests{0};
	std::atomic<std::uint32_t> connection_errors{0};
	std::atomic<bool> peer_valid{false};
	mnc::ipc::UnixStreamServer server(context.get_executor(), path.string());
	server.start([&](auto connection, auto frame) {
		const auto credentials = connection->peer_credentials();
		peer_valid = credentials.pid > 0 && credentials.uid == ::getuid();
		++requests;
		frame.kind = mnc::ipc::FrameKind::response;
		connection->post_send(std::move(frame));
	}, [&](const std::string &) { ++connection_errors; });
	std::thread worker([&] { context.run(); });

	try {
		mnc::ipc::BlockingClient client(path.string());
		const mnc::ipc::Frame request{mnc::ipc::FrameKind::request, 7, 42,
			{std::byte{9}, std::byte{8}}};
		const auto response = client.request(request, 1000);
		require(response.kind == mnc::ipc::FrameKind::response &&
			response.message_type == 7 && response.correlation_id == 42 &&
			response.payload == request.payload,
			"blocking IPC facade failed");

		const int split_socket = connect_raw(path);
		const auto split_wire = wire_frame(
			{mnc::ipc::FrameKind::request, 8, 43, {std::byte{4}}});
		write_all(split_socket, split_wire, 1);
		const auto split_response = read_raw_frame(split_socket);
		require(split_response.message_type == 8 &&
			split_response.correlation_id == 43,
			"frame fragmented at byte boundaries was not reassembled");
		::close(split_socket);

		const int joined_socket = connect_raw(path);
		auto joined = wire_frame(
			{mnc::ipc::FrameKind::request, 9, 44, {std::byte{5}}});
		auto second = wire_frame(
			{mnc::ipc::FrameKind::request, 10, 45, {std::byte{6}}});
		joined.insert(joined.end(), second.begin(), second.end());
		write_all(joined_socket, joined, joined.size());
		const auto first_response = read_raw_frame(joined_socket);
		const auto second_response = read_raw_frame(joined_socket);
		require(first_response.message_type == 9 &&
			second_response.message_type == 10,
			"coalesced IPC frames were not separated");
		::close(joined_socket);

		/* Product requests pass correlation zero and let the persistent
		 * transport allocate unique identifiers, including when independent
		 * HTTP-style callers submit requests concurrently. */
		auto persistent = std::make_shared<mnc::ipc::RequestClient>(
			context.get_executor(), path.string());
		boost::asio::co_spawn(context, persistent->connect(),
			boost::asio::use_future).get();
		auto first = boost::asio::co_spawn(context,
			persistent->request(
				{mnc::ipc::FrameKind::request, 11, 0, {std::byte{7}}},
				1000ms),
			boost::asio::use_future);
		auto second_request = boost::asio::co_spawn(context,
			persistent->request(
				{mnc::ipc::FrameKind::request, 12, 0, {std::byte{8}}},
				1000ms),
			boost::asio::use_future);
		const auto first_persistent = first.get();
		const auto second_persistent = second_request.get();
		require(first_persistent.correlation_id != 0 &&
			second_persistent.correlation_id != 0 &&
			first_persistent.correlation_id !=
				second_persistent.correlation_id,
			"persistent IPC client reused a correlation ID");
		persistent->close();

		for (int attempt = 0; attempt != 100 && requests.load() != 6;
		     ++attempt)
			std::this_thread::sleep_for(2ms);
		require(requests == 6 && peer_valid,
			"server did not process all frames or expose SO_PEERCRED");
		require(connection_errors == 0,
			"orderly client disconnect was reported as a server error");
	} catch (...) {
		server.stop();
		context.stop();
		worker.join();
		throw;
	}
	server.stop();
	context.stop();
	worker.join();
}

} // namespace

int main()
{
	try {
		byte_codec_and_envelope();
		fragmented_and_coalesced_streams();
		std::cout << "IPC tests passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "IPC test failed: " << error.what() << '\n';
		return 1;
	}
}
