#include "documentation_store.hpp"

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

namespace msap1::web::api {

std::optional<webengine::FileDownload> load_product_document(
	const std::filesystem::path &directory, std::string_view filename,
	std::string_view content_type)
{
	std::ifstream input(directory / filename, std::ios::binary);
	if (!input)
		return std::nullopt;
	std::string contents{std::istreambuf_iterator<char>(input),
		std::istreambuf_iterator<char>()};
	if (!input.eof() && input.fail())
		return std::nullopt;
	return webengine::FileDownload{std::string(filename),
		std::string(content_type), std::move(contents)};
}

std::filesystem::path product_documentation_directory()
{
	if (const auto *override_path =
		std::getenv("MSAP1_DOCUMENTATION_DIR");
	    override_path != nullptr && override_path[0] != '\0')
		return override_path;
	return product_documentation_path;
}

} // namespace msap1::web::api
