#pragma once

/**
 * @file openapi.hpp
 * @brief Typed OpenAPI contract shared by the web backend and build exporter.
 *
 * Runtime routes remain owned by routes.hpp.  The documented registry imports
 * that exact table, and each route module attaches the request/response DTOs
 * that are private to that module.  Contract generation therefore has no
 * second method/path allowlist, while route-local DTOs stay beside the code
 * that serializes them.
 */

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <glaze/glaze.hpp>
#include <webengine/Http.hpp>
#include <webengine/Role.hpp>

namespace msap1::web::api {

/**
 * A complete, validated catalogue of the externally supported HTTP contract.
 *
 * JSON schemas are produced from the same Glaze-compatible DTO types used by
 * handlers.  The implementation normalizes Glaze's JSON-Schema $defs into
 * OpenAPI components and emits deterministic OpenAPI 3.1 YAML.
 */
class DocumentedApiRegistry {
public:
	DocumentedApiRegistry();
	~DocumentedApiRegistry();
	DocumentedApiRegistry(DocumentedApiRegistry &&) noexcept;
	DocumentedApiRegistry &operator=(DocumentedApiRegistry &&) noexcept;
	DocumentedApiRegistry(const DocumentedApiRegistry &) = delete;
	DocumentedApiRegistry &operator=(const DocumentedApiRegistry &) = delete;

	void add_operation(webengine::http::verb method, std::string_view path,
		std::optional<webengine::Role> minimum_role,
		std::string_view summary, std::string_view tag = {},
		std::string_view operation_id = {});

	void add_query_parameter(webengine::http::verb method,
		std::string_view path, std::string_view name,
		std::string_view type, bool required, std::string_view description,
		std::vector<std::string> allowed_values = {},
		std::string_view example = {});
	void add_header_parameter(webengine::http::verb method,
		std::string_view path, std::string_view name,
		std::string_view type, bool required, std::string_view description,
		std::string_view example = {});
	void add_path_parameter(webengine::http::verb method,
		std::string_view path, std::string_view name,
		std::string_view type, std::string_view description,
		std::string_view example = {});

	template<typename T>
	void add_json_request(webengine::http::verb method,
		std::string_view path, std::string_view schema_name,
		std::string_view description, bool required = true,
		std::string_view example_json = {})
	{
		set_json_request(method, path, schema_name,
			json_schema<T>(schema_name), description, required,
			example_json);
	}

	template<typename T>
	void add_json_response(webengine::http::verb method,
		std::string_view path, unsigned status, std::string_view schema_name,
		std::string_view description, std::string_view example_json = {})
	{
		set_json_response(method, path, status, schema_name,
			json_schema<T>(schema_name), description, example_json);
	}

	void add_json_object_response(webengine::http::verb method,
		std::string_view path, unsigned status, std::string_view description,
		std::string_view example_json = {});
	void add_empty_response(webengine::http::verb method,
		std::string_view path, unsigned status, std::string_view description);
	void add_text_response(webengine::http::verb method,
		std::string_view path, unsigned status, std::string_view content_type,
		std::string_view description, std::string_view example = {});
	void add_error_response(webengine::http::verb method,
		std::string_view path, unsigned status, std::string_view description,
		std::string_view example_message = {});
	void add_binary_request(webengine::http::verb method,
		std::string_view path, std::vector<std::string> content_types,
		std::string_view description, bool required = true);
	void add_binary_response(webengine::http::verb method,
		std::string_view path, unsigned status, std::string_view content_type,
		std::string_view description, std::string_view filename = {},
		bool attachment = true);

	[[nodiscard]] bool contains(webengine::http::verb method,
		std::string_view path) const;
	[[nodiscard]] std::size_t operation_count() const;
	void validate() const;
	[[nodiscard]] std::string yaml() const;

private:
	template<typename T>
	static std::string json_schema(std::string_view schema_name)
	{
		auto schema = glz::write_json_schema<T>().value_or(std::string{});
		if (schema.empty())
			throw std::runtime_error(
				"Glaze could not generate schema " +
				std::string(schema_name));
		return schema;
	}

	void set_json_request(webengine::http::verb method,
		std::string_view path, std::string_view schema_name,
		std::string schema_json, std::string_view description,
		bool required, std::string_view example_json);
	void set_json_response(webengine::http::verb method,
		std::string_view path, unsigned status, std::string_view schema_name,
		std::string schema_json, std::string_view description,
		std::string_view example_json);
	void add_parameter(webengine::http::verb method,
		std::string_view path, std::string_view location,
		std::string_view name, std::string_view type, bool required,
		std::string_view description,
		std::vector<std::string> allowed_values,
		std::string_view example);

	struct Impl;
	std::unique_ptr<Impl> impl_;
};

/* Route-local schema decorators.  Each is defined beside its private DTOs. */
void document_health_routes(DocumentedApiRegistry &);
void document_attribute_routes(DocumentedApiRegistry &);
void document_meter_routes(DocumentedApiRegistry &);
void document_power_quality_routes(DocumentedApiRegistry &);
void document_energy_routes(DocumentedApiRegistry &);
void document_adc_routes(DocumentedApiRegistry &);
void document_waveform_routes(DocumentedApiRegistry &);
void document_settings_routes(DocumentedApiRegistry &);
void document_developer_routes(DocumentedApiRegistry &);
void document_database_routes(DocumentedApiRegistry &);
void document_history_routes(DocumentedApiRegistry &);
void document_mqtt_routes(DocumentedApiRegistry &);
void document_data_logging_routes(DocumentedApiRegistry &);
void document_documentation_routes(DocumentedApiRegistry &);

/** Build and validate the complete external API, then serialize it as YAML. */
[[nodiscard]] std::string generate_openapi_yaml();

} // namespace msap1::web::api
