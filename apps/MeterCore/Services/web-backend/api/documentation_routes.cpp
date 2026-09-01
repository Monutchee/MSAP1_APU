#include "documentation_store.hpp"
#include "openapi.hpp"
#include "routes.hpp"

namespace msap1::web::api {

std::optional<webengine::FileDownload> download_openapi_document(
	AppContext &, const webengine::RequestContext &)
{
	return load_product_document(product_documentation_directory(),
		openapi_document_filename, "application/yaml");
}

std::optional<webengine::FileDownload> download_modbus_document(
	AppContext &, const webengine::RequestContext &)
{
	return load_product_document(product_documentation_directory(),
		modbus_document_filename,
		"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet");
}

void document_documentation_routes(DocumentedApiRegistry &registry)
{
	using V = webengine::http::verb;
	registry.add_binary_response(V::get,
		"/api/v1/documentation/msap1_api.yaml", 200,
		"application/yaml", "OpenAPI 3.1 YAML built with this image",
		openapi_document_filename);
	registry.add_text_response(V::get,
		"/api/v1/documentation/msap1_api.yaml", 404,
		"text/plain", "The packaged OpenAPI document is missing",
		"file is not configured");

	registry.add_binary_response(V::get,
		"/api/v1/documentation/msap1_modbus_registers.xlsx", 200,
		"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
		"Modbus register workbook built with this image",
		modbus_document_filename);
	registry.add_text_response(V::get,
		"/api/v1/documentation/msap1_modbus_registers.xlsx", 404,
		"text/plain", "The packaged Modbus workbook is missing",
		"file is not configured");
}

} // namespace msap1::web::api
