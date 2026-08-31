#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

namespace msap1::web::api {

inline unsigned query_hex_digit(char value)
{
	if (value >= '0' && value <= '9')
		return static_cast<unsigned>(value - '0');
	if (value >= 'a' && value <= 'f')
		return static_cast<unsigned>(value - 'a' + 10);
	if (value >= 'A' && value <= 'F')
		return static_cast<unsigned>(value - 'A' + 10);
	throw std::invalid_argument("invalid URL encoding");
}

inline std::string url_decode(std::string_view value)
{
	std::string result;
	result.reserve(value.size());
	for (std::size_t index = 0; index < value.size(); ++index) {
		if (value[index] == '+') {
			result.push_back(' ');
			continue;
		}
		if (value[index] != '%') {
			result.push_back(value[index]);
			continue;
		}
		if (index + 2u >= value.size())
			throw std::invalid_argument("invalid URL encoding");
		const auto byte = (query_hex_digit(value[index + 1u]) << 4u) |
			query_hex_digit(value[index + 2u]);
		result.push_back(static_cast<char>(byte));
		index += 2u;
	}
	return result;
}

/** Decode a request target's query and reject duplicate parameter names. */
inline std::unordered_map<std::string, std::string>
query_parameters(std::string_view target)
{
	std::unordered_map<std::string, std::string> result;
	const auto question = target.find('?');
	if (question == std::string_view::npos)
		return result;
	auto query = target.substr(question + 1u);
	while (!query.empty()) {
		const auto separator = query.find('&');
		const auto item = query.substr(0, separator);
		const auto equals = item.find('=');
		const auto name = url_decode(item.substr(0, equals));
		const auto value = equals == std::string_view::npos
			? std::string{} : url_decode(item.substr(equals + 1u));
		if (!name.empty() && !result.emplace(name, value).second)
			throw std::invalid_argument("duplicate query parameter: " + name);
		if (separator == std::string_view::npos)
			break;
		query.remove_prefix(separator + 1u);
	}
	return result;
}

} // namespace msap1::web::api
