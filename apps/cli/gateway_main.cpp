#include "cli.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

/*
 * Parse SSH_ORIGINAL_COMMAND without invoking a shell. Quotes are supported
 * solely for grouping values such as --since "10 minutes ago"; shell
 * expansion, redirection, pipelines, substitutions, and control operators are
 * deliberately rejected.
 */
std::vector<std::string> tokenize(std::string_view command)
{
	enum class Quote { none, single, double_quote };
	Quote quote = Quote::none;
	std::vector<std::string> result;
	std::string token;
	bool token_started = false;

	auto finish_token = [&] {
		if (token_started) {
			result.push_back(std::move(token));
			token.clear();
			token_started = false;
		}
	};

	for (std::size_t index = 0; index < command.size(); ++index) {
		const auto character = command[index];
		if (quote == Quote::none) {
			if (character == ' ' || character == '\t') {
				finish_token();
				continue;
			}
			if (character == '\'' || character == '"') {
				quote = character == '\'' ? Quote::single
							 : Quote::double_quote;
				token_started = true;
				continue;
			}
			if (character == '\\') {
				if (++index >= command.size())
					throw std::invalid_argument(
						"trailing command escape");
				token.push_back(command[index]);
				token_started = true;
				continue;
			}
			if (std::string_view{";&|<>`$()\r\n"}.find(character) !=
			    std::string_view::npos)
				throw std::invalid_argument(
					"shell operators are not permitted");
			token.push_back(character);
			token_started = true;
			continue;
		}
		if ((quote == Quote::single && character == '\'') ||
		    (quote == Quote::double_quote && character == '"')) {
			quote = Quote::none;
			continue;
		}
		if (quote == Quote::double_quote && character == '\\') {
			if (++index >= command.size())
				throw std::invalid_argument(
					"trailing quoted command escape");
			token.push_back(command[index]);
			continue;
		}
		token.push_back(character);
	}
	if (quote != Quote::none)
		throw std::invalid_argument("unterminated command quote");
	finish_token();
	return result;
}

} // namespace

int main()
{
	const char *original = std::getenv("SSH_ORIGINAL_COMMAND");
	if (original == nullptr || *original == '\0') {
		msap1::cli::write_json_error(
			std::cout, "INTERACTIVE_ACCESS_DENIED",
			"debug access accepts only non-interactive mnc commands");
		return 3;
	}

	try {
		auto arguments = tokenize(original);
		if (arguments.empty() || arguments.front() != "mnc") {
			msap1::cli::write_json_error(
				std::cout, "COMMAND_DENIED",
				"restricted access permits only the mnc command");
			return 3;
		}
		arguments.erase(arguments.begin());
		const auto application = msap1::cli::make_application();
		return application.execute(
			arguments, std::cout, std::cerr,
			{
				.maximum_access =
					msap1::cli::AccessLevel::diagnostic,
				.require_json = true,
				.allow_socket_override = false,
				.allow_timeout_override = false,
			});
	} catch (const std::exception &error) {
		msap1::cli::write_json_error(std::cout, "COMMAND_INVALID",
					    error.what());
		return 2;
	}
}
