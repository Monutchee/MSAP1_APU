#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

#include <webengine/Http.hpp>

namespace msap1::web::api {

inline constexpr std::string_view product_documentation_path =
	"/usr/share/monutchee/msap1/docs";
inline constexpr std::string_view openapi_document_filename = "msap1_api.yaml";
inline constexpr std::string_view modbus_document_filename =
	"msap1_modbus_registers.xlsx";

/** Read one immutable packaged document without generating or modifying it. */
[[nodiscard]] std::optional<webengine::FileDownload> load_product_document(
	const std::filesystem::path &directory, std::string_view filename,
	std::string_view content_type);

/** Rootfs directory, overridable only to make host tests hermetic. */
[[nodiscard]] std::filesystem::path product_documentation_directory();

} // namespace msap1::web::api
