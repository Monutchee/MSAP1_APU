#include "mnc/modbus/request_handler.hpp"

#include "mnc/modbus/encoding.hpp"

#include <cstddef>
#include <stdexcept>
#include <utility>

namespace mnc::modbus {
namespace {

std::uint8_t octet(std::byte value)
{
	return std::to_integer<std::uint8_t>(value);
}

} // namespace

std::vector<std::byte> RequestHandler::exception(
	std::uint8_t function, ExceptionCode code) const
{
	return {static_cast<std::byte>(function | 0x80u),
		static_cast<std::byte>(code)};
}

std::optional<Response> RequestHandler::handle(
	const Request &request, std::uint8_t configured_unit_id)
{
	const auto pdu = request.pdu;
	if (configured_unit_id == 0 || configured_unit_id > 247)
		throw std::invalid_argument("Modbus unit id must be 1..247");
	auto response = [&](std::vector<std::byte> value) {
		return std::optional<Response>{Response{request.unit_id,
			std::move(value)}};
	};
	if (pdu.empty())
		return response(exception(0, ExceptionCode::illegal_function));
	const auto function = octet(pdu[0]);
	const bool broadcast = request.broadcast_allowed && request.unit_id == 0;
	if (!broadcast && request.unit_id != configured_unit_id)
		return std::nullopt;

	auto require_size = [&](std::size_t size) {
		return pdu.size() == size;
	};
	auto address = [&] { return read_u16_be(pdu.subspan(1, 2)); };
	auto quantity = [&] { return read_u16_be(pdu.subspan(3, 2)); };

	switch (static_cast<FunctionCode>(function)) {
	case FunctionCode::read_holding_registers:
	case FunctionCode::read_input_registers: {
		if (broadcast)
			return std::nullopt;
		if (!require_size(5))
			return response(exception(function,
				ExceptionCode::illegal_data_value));
		const auto count = quantity();
		if (count == 0 || count > 125)
			return response(exception(function,
				ExceptionCode::illegal_data_value));
		auto result = registers_.read(static_cast<FunctionCode>(function),
			address(), count);
		if (result.exception != ExceptionCode::none)
			return response(exception(function, result.exception));
		if (result.values.size() != count)
			return response(exception(function,
				ExceptionCode::server_device_failure));
		std::vector<std::byte> response_pdu;
		response_pdu.reserve(2 + result.values.size() * 2);
		response_pdu.push_back(static_cast<std::byte>(function));
		response_pdu.push_back(
			static_cast<std::byte>(result.values.size() * 2));
		for (const auto value : result.values)
			append_u16_be(response_pdu, value);
		return Response{request.unit_id, std::move(response_pdu)};
	}
	case FunctionCode::write_single_register: {
		if (!require_size(5)) {
			if (broadcast)
				return std::nullopt;
			return response(exception(function,
				ExceptionCode::illegal_data_value));
		}
		const auto code = registers_.write_single(address(), quantity());
		if (broadcast)
			return std::nullopt;
		if (code != ExceptionCode::none)
			return response(exception(function, code));
		return Response{request.unit_id,
			std::vector<std::byte>(pdu.begin(), pdu.end())};
	}
	case FunctionCode::write_multiple_registers: {
		if (pdu.size() < 6) {
			if (broadcast)
				return std::nullopt;
			return response(exception(function,
				ExceptionCode::illegal_data_value));
		}
		const auto count = quantity();
		const auto byte_count = octet(pdu[5]);
		if (count == 0 || count > 123 || byte_count != count * 2 ||
		    pdu.size() != static_cast<std::size_t>(6 + byte_count)) {
			if (broadcast)
				return std::nullopt;
			return response(exception(function,
				ExceptionCode::illegal_data_value));
		}
		std::vector<std::uint16_t> values;
		values.reserve(count);
		for (std::uint16_t index = 0; index < count; ++index)
			values.push_back(
				read_u16_be(pdu.subspan(6 + index * 2, 2)));
		const auto code = registers_.write_multiple(address(), values);
		if (broadcast)
			return std::nullopt;
		if (code != ExceptionCode::none)
			return response(exception(function, code));
		std::vector<std::byte> response_pdu{
			static_cast<std::byte>(function)};
		append_u16_be(response_pdu, address());
		append_u16_be(response_pdu, count);
		return Response{request.unit_id, std::move(response_pdu)};
	}
	default:
		if (broadcast)
			return std::nullopt;
		return response(exception(function,
			ExceptionCode::illegal_function));
	}
}

} // namespace mnc::modbus
