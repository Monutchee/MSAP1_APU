#include "api/openapi.hpp"

#include <exception>
#include <iostream>
#include <string>
#include <string_view>

namespace {

void require_fragment(const std::string &document, std::string_view fragment)
{
	if (!document.contains(fragment))
		throw std::runtime_error("generated OpenAPI omits required fragment: " +
			std::string(fragment));
}

void check_contract()
{
	const auto first = msap1::web::api::generate_openapi_yaml();
	const auto second = msap1::web::api::generate_openapi_yaml();
	if (first.empty())
		throw std::runtime_error("generated OpenAPI document is empty");
	if (first != second)
		throw std::runtime_error("OpenAPI generation is not deterministic");
	require_fragment(first, "openapi: '3.1.0'");
	require_fragment(first, "sessionCookie");
	require_fragment(first, "x-msap1-minimum-role");
	require_fragment(first, "/api/login");
	require_fragment(first,
		"/api/v1/documentation/msap1_api.yaml");
	require_fragment(first,
		"/api/v1/documentation/msap1_modbus_registers.xlsx");
	require_fragment(first, "/api/v1/meter/frequency-10s");
	require_fragment(first, "/protected/waveforms/view/{filename}");
}

} // namespace

int main(int argc, char **argv)
{
	try {
		if (argc == 2 && std::string_view(argv[1]) == "--check") {
			check_contract();
			return 0;
		}
		if (argc != 1) {
			std::cerr << "usage: msap1-openapi-dump [--check]\n";
			return 2;
		}
		std::cout << msap1::web::api::generate_openapi_yaml();
		return std::cout.good() ? 0 : 1;
	} catch (const std::exception &error) {
		std::cerr << "msap1-openapi-dump: " << error.what() << '\n';
		return 1;
	}
}
