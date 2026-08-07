#include "core/cli.hpp"

#include <csignal>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

namespace {

void handle_signal(int) { msap1::cli::request_stop(); }

} // namespace

int main(int argc, char **argv)
{
	std::signal(SIGINT, handle_signal);
	std::signal(SIGTERM, handle_signal);

	std::vector<std::string> arguments;
	arguments.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0);
	for (int index = 1; index < argc; ++index)
		arguments.emplace_back(argv[index]);

	try {
		const auto application = msap1::cli::make_application();
		if (!arguments.empty() && arguments.front() == "__complete") {
			arguments.erase(arguments.begin());
			for (const auto &candidate : application.complete(arguments))
				std::cout << candidate << '\n';
			return 0;
		}
		return application.execute(arguments, std::cout, std::cerr);
	} catch (const std::exception &error) {
		std::cerr << "mnc: " << error.what() << '\n';
		return 1;
	}
}
