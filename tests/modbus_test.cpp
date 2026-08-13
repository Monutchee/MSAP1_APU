#include "mnc/modbus/modbus.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <array>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using namespace mnc::modbus;

void require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

std::uint8_t octet(std::byte value)
{
	return std::to_integer<std::uint8_t>(value);
}

class TestBank final : public RegisterBank {
public:
	RegisterReadResult read(RegisterTable table, std::uint16_t address,
		std::uint16_t count) const override
	{
		if (read_exception != ExceptionCode::none)
			return {read_exception, {}};
		const auto &source = table == RegisterTable::input ? input : holding;
		if (static_cast<std::size_t>(address) + count > source.size())
			return {ExceptionCode::illegal_data_address, {}};
		return {ExceptionCode::none,
			{source.begin() + address, source.begin() + address + count}};
	}

	ExceptionCode write_single(std::uint16_t address,
		std::uint16_t value) override
	{
		if (address >= holding.size())
			return ExceptionCode::illegal_data_address;
		holding[address] = value;
		return ExceptionCode::none;
	}

	ExceptionCode write_multiple(std::uint16_t address,
		std::span<const std::uint16_t> values) override
	{
		if (static_cast<std::size_t>(address) + values.size() > holding.size())
			return ExceptionCode::illegal_data_address;
		std::copy(values.begin(), values.end(), holding.begin() + address);
		return ExceptionCode::none;
	}

	mutable std::array<std::uint16_t, 8> holding{0x1000, 0x1001};
	std::array<std::uint16_t, 8> input{0x2000, 0x2001, 0x2002};
	ExceptionCode read_exception = ExceptionCode::none;
};

std::vector<std::byte> request(std::uint8_t function,
	std::initializer_list<std::uint8_t> payload)
{
	std::vector<std::byte> value{static_cast<std::byte>(function)};
	for (const auto byte : payload)
		value.push_back(static_cast<std::byte>(byte));
	return value;
}

void protocol_test()
{
	TestBank bank;
	RequestHandler handler(bank);

	auto pdu = request(0x04, {0, 0, 0, 3});
	auto reply = handler.handle(Request{1, pdu, false}, 1);
	require(reply && reply->pdu.size() == 8, "FC04 response size changed");
	require(octet(reply->pdu[0]) == 0x04 && octet(reply->pdu[1]) == 6,
		"FC04 response header changed");
	require(read_u16_be(std::span(reply->pdu).subspan(2, 2)) == 0x2000,
		"FC04 register encoding changed");

	pdu = request(0x03, {0, 0, 0, 2});
	reply = handler.handle(Request{1, pdu, false}, 1);
	require(reply && read_u16_be(std::span(reply->pdu).subspan(2, 2)) == 0x1000,
		"FC03 register read failed");

	pdu = request(0x06, {0, 1, 0xab, 0xcd});
	reply = handler.handle(Request{1, pdu, false}, 1);
	require(reply && bank.holding[1] == 0xabcd, "FC06 register write failed");

	pdu = request(0x10, {0, 2, 0, 2, 4, 0x12, 0x34, 0x56, 0x78});
	reply = handler.handle(Request{1, pdu, false}, 1);
	require(reply && bank.holding[2] == 0x1234 && bank.holding[3] == 0x5678,
		"FC10 register write failed");

	pdu = request(0x7f, {0, 0, 0, 1});
	reply = handler.handle(Request{1, pdu, false}, 1);
	require(reply && octet(reply->pdu[0]) == 0xff && octet(reply->pdu[1]) == 1,
		"illegal function was not rejected");

	pdu = request(0x04, {0, 7, 0, 2});
	reply = handler.handle(Request{1, pdu, false}, 1);
	require(reply && octet(reply->pdu[0]) == 0x84 && octet(reply->pdu[1]) == 2,
		"illegal address was not rejected");

	require(!handler.handle(Request{2, pdu, false}, 1),
		"different unit ID received a response");
	const auto prior = bank.holding[0];
	pdu = request(0x06, {0, 0, 0x44, 0x55});
	require(!handler.handle(Request{0, pdu, true}, 1),
		"RTU broadcast incorrectly received a response");
	require(bank.holding[0] == 0x4455 && prior != bank.holding[0],
		"RTU broadcast write was not applied");
}

void exception_code_test()
{
	TestBank bank;
	RequestHandler handler(bank);
	const auto pdu = request(0x04, {0, 0, 0, 1});
	constexpr std::array codes{
		ExceptionCode::illegal_function,
		ExceptionCode::illegal_data_address,
		ExceptionCode::illegal_data_value,
		ExceptionCode::server_device_failure,
		ExceptionCode::acknowledge,
		ExceptionCode::server_device_busy,
		ExceptionCode::memory_parity_error,
		ExceptionCode::gateway_path_unavailable,
		ExceptionCode::gateway_target_device_failed_to_respond,
	};

	for (const auto code : codes) {
		bank.read_exception = code;
		const auto reply = handler.handle(Request{1, pdu, false}, 1);
		require(reply && reply->pdu.size() == 2,
			"backend exception response size changed");
		require(octet(reply->pdu[0]) == 0x84,
			"backend exception function code changed");
		require(octet(reply->pdu[1]) == static_cast<std::uint8_t>(code),
			"backend exception code was not preserved");
	}
}

void encoding_test()
{
	require(encode_u16(0xabcd) ==
		(std::vector<std::uint16_t>{0xabcd}), "uint16 encoding changed");
	require(encode_u32(0x12345678u) ==
		(std::vector<std::uint16_t>{0x1234, 0x5678}),
		"uint32 high-word-first encoding changed");
	require(encode_u64(0x0123456789abcdefu) ==
		(std::vector<std::uint16_t>{0x0123, 0x4567, 0x89ab, 0xcdef}),
		"uint64 high-word-first encoding changed");
	require(encode_i32(-2) ==
		(std::vector<std::uint16_t>{0xffff, 0xfffe}),
		"int32 high-word-first encoding changed");
	require(encode_float(1.0f) ==
		(std::vector<std::uint16_t>{0x3f80, 0x0000}),
		"float32 high-word-first encoding changed");
	const std::array<std::byte, 6> known{
		std::byte{0x01}, std::byte{0x03}, std::byte{0x00},
		std::byte{0x00}, std::byte{0x00}, std::byte{0x0a}};
	require(crc16(known) == 0xcdc5, "Modbus CRC implementation changed");
}

void rtu_assembler_test()
{
	std::vector<std::byte> first{
		std::byte{1}, std::byte{3}, std::byte{0}, std::byte{0},
		std::byte{0}, std::byte{1}, std::byte{0x84}, std::byte{0x0a}};
	std::vector<std::byte> second{
		std::byte{1}, std::byte{6}, std::byte{0}, std::byte{1},
		std::byte{0}, std::byte{2}, std::byte{0x59}, std::byte{0xcb}};
	require(valid_crc(first), "known RTU CRC was rejected");
	require(!valid_crc(std::span(first).first(7)), "truncated CRC was accepted");
	RtuFrameAssembler assembler;
	require(assembler.push(std::span(first).first(3)).empty(),
		"partial RTU frame was emitted");
	auto frames = assembler.push(std::span(first).subspan(3));
	require(frames.size() == 1 && frames[0] == first,
		"fragmented RTU frame assembly failed");
	first.insert(first.end(), second.begin(), second.end());
	frames = assembler.push(first);
	require(frames.size() == 2, "coalesced RTU frames were not split");

	RtuFrameAssembler first_port;
	RtuFrameAssembler second_port;
	require(first_port.push(std::span(first).first(2)).empty(),
		"first RTU port emitted an incomplete frame");
	require(second_port.push(second).size() == 1,
		"one RTU port was blocked by another port's partial frame");
	require(!first_port.empty() && second_port.empty(),
		"RTU port assemblers did not retain independent state");
	first_port.clear();
	require(first_port.empty(), "RTU silent-interval reset did not clear state");
}

std::vector<std::byte> tcp_adu(std::uint16_t transaction,
	std::span<const std::byte> pdu)
{
	std::vector<std::byte> value;
	append_u16_be(value, transaction);
	append_u16_be(value, 0);
	append_u16_be(value, static_cast<std::uint16_t>(pdu.size() + 1));
	value.push_back(std::byte{1});
	value.insert(value.end(), pdu.begin(), pdu.end());
	return value;
}

void tcp_test()
{
	TestBank bank;
	RequestHandler handler(bank);
	boost::asio::io_context server_io;
	ModbusTcpServer server(server_io.get_executor(), handler,
		{"127.0.0.1", 0, 4});
	server.start();
	std::thread server_thread([&] { server_io.run(); });

	auto one_client = [&](std::uint16_t transaction, bool repeat) {
		boost::asio::io_context client_io;
		boost::asio::ip::tcp::socket socket(client_io);
		socket.connect({boost::asio::ip::make_address("127.0.0.1"),
			server.local_port()});
		const auto pdu = request(0x04, {0, 0, 0, 1});
		const auto frame = tcp_adu(transaction, pdu);
		/* Deliberately split MBAP and payload: TCP has no message boundaries. */
		boost::asio::write(socket, boost::asio::buffer(frame.data(), 3));
		boost::asio::write(socket,
			boost::asio::buffer(frame.data() + 3, frame.size() - 3));
		std::array<std::byte, 11> reply{};
		boost::asio::read(socket, boost::asio::buffer(reply));
		require(read_u16_be(reply) == transaction,
			"TCP transaction ID was not preserved");
		require(octet(reply[7]) == 0x04 && octet(reply[8]) == 2,
			"TCP response PDU changed");
		if (repeat) {
			const auto next = tcp_adu(transaction + 1, pdu);
			boost::asio::write(socket, boost::asio::buffer(next));
			boost::asio::read(socket, boost::asio::buffer(reply));
			require(read_u16_be(reply) == transaction + 1,
				"persistent TCP connection lost request ordering");
		}
	};
	std::thread first([&] { one_client(0x1234, true); });
	std::thread second([&] { one_client(0x5678, false); });
	first.join();
	second.join();
	server.stop();
	server_io.stop();
	server_thread.join();
}

} // namespace

int main()
{
	protocol_test();
	exception_code_test();
	encoding_test();
	rtu_assembler_test();
	tcp_test();
}
