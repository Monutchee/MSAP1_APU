#include "api/documentation_store.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <unistd.h>

int main()
{
	namespace fs = std::filesystem;
	const auto directory = fs::temp_directory_path() /
		("msap1-documentation-store-" + std::to_string(::getpid()));
	std::error_code ignored;
	fs::remove_all(directory, ignored);
	if (!fs::create_directories(directory)) {
		std::cerr << "could not create test documentation directory\n";
		return 1;
	}

	const auto filename = std::string{
		msap1::web::api::openapi_document_filename};
	{
		std::ofstream output(directory / filename, std::ios::binary);
		output << "openapi: 3.1.0\n";
	}
	const auto loaded = msap1::web::api::load_product_document(directory,
		filename, "application/yaml");
	const bool valid = loaded && loaded->file_name == filename &&
		loaded->content_type == "application/yaml" &&
		loaded->contents == "openapi: 3.1.0\n";
	const auto missing = msap1::web::api::load_product_document(directory,
		"missing.xlsx",
		"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet");
	fs::remove_all(directory, ignored);
	if (!valid || missing) {
		std::cerr << "packaged-document success/missing behavior changed\n";
		return 1;
	}
	return 0;
}
