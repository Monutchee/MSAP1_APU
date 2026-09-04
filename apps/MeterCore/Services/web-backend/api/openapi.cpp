#include "openapi.hpp"

#include "routes.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

#include <glaze/yaml.hpp>

namespace msap1::web::api {
namespace {

using Json = glz::generic_sorted;
using Verb = webengine::http::verb;

struct ErrorResponseDto {
	std::string error;
	std::optional<std::string> code;
	std::optional<bool> retryable;
	std::optional<std::vector<std::string>> missing_fields;
};

struct LoginRequestDto {
	std::string username;
	std::string password;
};

struct StatusResponseDto {
	std::string status;
};

struct ParameterData {
	std::string name;
	std::string location;
	std::string type;
	bool required = false;
	std::string description;
	std::vector<std::string> allowed_values;
	std::string example;
};

struct BodyData {
	std::string description;
	bool required = true;
	std::vector<std::string> content_types;
	std::optional<std::string> schema_name;
	bool binary = false;
	std::string example_json;
};

struct ResponseData {
	std::string description;
	std::optional<std::string> content_type;
	std::optional<std::string> schema_name;
	bool binary = false;
	bool attachment = false;
	std::string filename;
	std::string example_json;
};

struct OperationData {
	Verb method{};
	std::string path;
	std::optional<webengine::Role> minimum_role;
	std::string summary;
	std::string tag;
	std::string operation_id;
	std::vector<ParameterData> parameters;
	std::optional<BodyData> request;
	std::map<unsigned, ResponseData> responses;
};

using OperationKey = std::pair<std::string, std::string>;

std::string method_name(Verb method)
{
	switch (method) {
	case Verb::get: return "get";
	case Verb::put: return "put";
	case Verb::post: return "post";
	case Verb::delete_: return "delete";
	case Verb::patch: return "patch";
	default: break;
	}
	throw std::invalid_argument("unsupported OpenAPI HTTP method");
}

std::string openapi_role_name(webengine::Role role)
{
	return role == webengine::Role::Admin ? "admin" : "viewer";
}

std::string derive_tag(std::string_view path)
{
	if (path.starts_with("/protected/waveforms/")) return "Waveforms";
	if (path == "/api/login" || path == "/api/logout")
		return "Authentication";
	if (path == "/api/v1/session" || path == "/api/v1/health" ||
	    path == "/api/v1/about")
		return "System";
	constexpr std::string_view prefix = "/api/v1/";
	if (!path.starts_with(prefix)) return "External";
	auto component = path.substr(prefix.size());
	component = component.substr(0, component.find('/'));
	if (component == "meter") return "Metering";
	if (component == "adc") return "ADC";
	if (component == "settings") return "Settings";
	if (component == "developer") return "Developer";
	if (component == "mqtt") return "MQTT";
	if (component == "data-logging") return "Data Logging";
	if (component == "waveforms") return "Waveforms";
	if (component == "documentation") return "Documentation";
	return "System";
}

std::string derive_operation_id(Verb method, std::string_view path)
{
	std::string result = method_name(method);
	for (const unsigned char character : path) {
		if (std::isalnum(character)) {
			result.push_back('_');
			result.push_back(static_cast<char>(std::tolower(character)));
		} else if (!result.empty() && result.back() != '_') {
			result.push_back('_');
		}
	}
	while (!result.empty() && result.back() == '_') result.pop_back();
	return result;
}

Json json_array(std::vector<Json> values)
{
	Json result;
	result = Json::array_t(std::make_move_iterator(values.begin()),
		std::make_move_iterator(values.end()));
	return result;
}

Json string_array(const std::vector<std::string> &values)
{
	std::vector<Json> result;
	result.reserve(values.size());
	for (const auto &value : values) result.emplace_back(value);
	return json_array(std::move(result));
}

Json schema_reference(std::string_view name)
{
	Json result;
	result["$ref"] = "#/components/schemas/" + std::string(name);
	return result;
}

Json binary_schema()
{
	Json result;
	result["type"] = "string";
	result["format"] = "binary";
	return result;
}

Json string_schema()
{
	Json result;
	result["type"] = "string";
	return result;
}

Json parse_json(std::string_view input, std::string_view purpose)
{
	Json result;
	if (const auto error = glz::read_json(result, input))
		throw std::runtime_error("invalid JSON " + std::string(purpose) +
			": " + glz::format_error(error, input));
	return result;
}

void rewrite_schema_references(Json &value, std::string_view prefix)
{
	if (auto *object = value.get_if<Json::object_t>()) {
		if (const auto found = object->find("$ref"); found != object->end() &&
		    found->second.is_string()) {
			auto &reference = found->second.get<std::string>();
			constexpr std::string_view defs = "#/$defs/";
			if (reference.starts_with(defs))
				reference = "#/components/schemas/" +
					std::string(prefix) + "_" +
					reference.substr(defs.size());
		}
		for (auto &[name, child] : *object) {
			(void)name;
			rewrite_schema_references(child, prefix);
		}
	} else if (auto *array = value.get_if<Json::array_t>()) {
		for (auto &child : *array)
			rewrite_schema_references(child, prefix);
	}
}

std::string dump_json(const Json &value)
{
	return glz::write_json(value).value_or(std::string{});
}

OperationKey operation_key(Verb method, std::string_view path)
{
	return {method_name(method), std::string(path)};
}

Json parameter_schema(const ParameterData &parameter)
{
	Json schema;
	schema["type"] = parameter.type;
	if (!parameter.allowed_values.empty())
		schema["enum"] = string_array(parameter.allowed_values);
	return schema;
}

void add_external_operations(DocumentedApiRegistry &registry)
{
	registry.add_operation(Verb::post, "/api/login", std::nullopt,
		"Authenticate and issue an HTTP-only session cookie",
		"Authentication", "login");
	registry.add_json_request<LoginRequestDto>(Verb::post, "/api/login",
		"LoginRequest", "Development or deployed user credentials", true,
		R"({"username":"admin","password":"admin"})");
	registry.add_json_response<StatusResponseDto>(Verb::post, "/api/login",
		200, "StatusResponse", "Session cookie issued",
		R"({"status":"ok"})");
	registry.add_error_response(Verb::post, "/api/login", 401,
		"Credentials were rejected", "invalid credentials");

	registry.add_operation(Verb::post, "/api/logout", std::nullopt,
		"Revoke the current session cookie", "Authentication", "logout");
	registry.add_json_response<StatusResponseDto>(Verb::post, "/api/logout",
		200, "StatusResponse", "Session cookie revoked",
		R"({"status":"ok"})");

	registry.add_operation(Verb::get, "/api/v1/waveforms/export",
		webengine::Role::Viewer,
		"Stream a virtual MNCWF event capture", "Waveforms");
	registry.add_operation(Verb::get,
		"/api/v1/waveform-exports/download", webengine::Role::Viewer,
		"Stream one ready converted waveform artifact", "Waveforms");
	registry.add_operation(Verb::get,
		"/api/v1/data-logging/artifacts/download", webengine::Role::Viewer,
		"Download one manifest-authorized generated artifact",
		"Data Logging");

	for (const auto path : {"/api/v1/mqtt/tls/ca",
		"/api/v1/mqtt/tls/client-certificate"}) {
		registry.add_operation(Verb::put, path, webengine::Role::Admin,
			"Upload an MQTT TLS credential asset", "MQTT");
		registry.add_operation(Verb::get, path, webengine::Role::Admin,
			"Download an installed MQTT public credential asset", "MQTT");
	}
	registry.add_operation(Verb::put, "/api/v1/mqtt/tls/client-key",
		webengine::Role::Admin, "Upload the MQTT client private key", "MQTT");
	registry.add_operation(Verb::put,
		"/api/v1/data-logging/channel-asset", webengine::Role::Admin,
		"Upload one Data Logging channel verification or identity asset",
		"Data Logging");

	registry.add_operation(Verb::get,
		"/api/v1/documentation/msap1_api.yaml", webengine::Role::Viewer,
		"Download the OpenAPI contract built into this image",
		"Documentation");
	registry.add_operation(Verb::get,
		"/api/v1/documentation/msap1_modbus_registers.xlsx",
		webengine::Role::Viewer,
		"Download the Modbus register workbook built into this image",
		"Documentation");

	for (const auto &[path, summary] : {
		std::pair{"/protected/waveforms/view/{filename}",
			"View one retained MNCWF capture"},
		std::pair{"/protected/waveforms/download/{filename}",
			"Download one retained MNCWF capture"}}) {
		registry.add_operation(Verb::get, path, webengine::Role::Viewer,
			summary, "Waveforms");
		registry.add_path_parameter(Verb::get, path, "filename", "string",
			"Safe retained .mncwf filename", "capture-1.mncwf");
	}
}

} // namespace

struct DocumentedApiRegistry::Impl {
	std::map<OperationKey, OperationData> operations;
	std::map<std::string, Json> schemas;

	OperationData &operation(Verb method, std::string_view path)
	{
		const auto found = operations.find(operation_key(method, path));
		if (found == operations.end())
			throw std::invalid_argument("OpenAPI operation is not registered: " +
				method_name(method) + " " + std::string(path));
		return found->second;
	}

	const OperationData &operation(Verb method, std::string_view path) const
	{
		const auto found = operations.find(operation_key(method, path));
		if (found == operations.end())
			throw std::invalid_argument("OpenAPI operation is not registered: " +
				method_name(method) + " " + std::string(path));
		return found->second;
	}

	void add_schema(std::string_view name, std::string_view schema_json)
	{
		if (name.empty())
			throw std::invalid_argument("OpenAPI schema name is empty");
		Json root = parse_json(schema_json, "schema " + std::string(name));
		if (!root.is_object())
			throw std::invalid_argument("OpenAPI schema is not an object: " +
				std::string(name));

		auto &object = root.get<Json::object_t>();
		object.erase("$schema");
		std::vector<std::pair<std::string, Json>> definitions;
		if (const auto found = object.find("$defs"); found != object.end()) {
			if (!found->second.is_object())
				throw std::invalid_argument("OpenAPI schema $defs is not an object");
			for (auto &[definition_name, definition] :
			     found->second.get<Json::object_t>())
				definitions.emplace_back(std::string(name) + "_" +
					definition_name, std::move(definition));
			object.erase(found);
		}
		rewrite_schema_references(root, name);
		for (auto &[definition_name, definition] : definitions)
			rewrite_schema_references(definition, name);

		const auto insert = [&](std::string schema_name, Json schema) {
			const auto found = schemas.find(schema_name);
			if (found != schemas.end()) {
				if (dump_json(found->second) != dump_json(schema))
					throw std::invalid_argument(
						"conflicting OpenAPI schema name: " + schema_name);
				return;
			}
			schemas.emplace(std::move(schema_name), std::move(schema));
		};
		insert(std::string(name), std::move(root));
		for (auto &[definition_name, definition] : definitions)
			insert(std::move(definition_name), std::move(definition));
	}
};

DocumentedApiRegistry::DocumentedApiRegistry()
	: impl_(std::make_unique<Impl>())
{
	impl_->add_schema("ErrorResponse",
		json_schema<ErrorResponseDto>("ErrorResponse"));
	Json object_schema;
	object_schema["type"] = "object";
	object_schema["additionalProperties"] = true;
	impl_->schemas.emplace("JsonObject", std::move(object_schema));
}

DocumentedApiRegistry::~DocumentedApiRegistry() = default;
DocumentedApiRegistry::DocumentedApiRegistry(DocumentedApiRegistry &&) noexcept = default;
DocumentedApiRegistry &DocumentedApiRegistry::operator=(
	DocumentedApiRegistry &&) noexcept = default;

void DocumentedApiRegistry::add_operation(Verb method, std::string_view path,
	std::optional<webengine::Role> minimum_role, std::string_view summary,
	std::string_view tag, std::string_view operation_id)
{
	if (path.empty() || path.front() != '/' || summary.empty())
		throw std::invalid_argument("incomplete OpenAPI operation metadata");
	OperationData operation{
		.method = method,
		.path = std::string(path),
		.minimum_role = minimum_role,
		.summary = std::string(summary),
		.tag = tag.empty() ? derive_tag(path) : std::string(tag),
		.operation_id = operation_id.empty()
			? derive_operation_id(method, path) : std::string(operation_id),
		.parameters = {},
		.request = std::nullopt,
		.responses = {},
	};
	if (minimum_role) {
		operation.responses.emplace(401, ResponseData{
			.description = "Authentication is required",
			.content_type = "application/json",
			.schema_name = "ErrorResponse",
			.binary = false,
			.attachment = false,
			.filename = {},
			.example_json = {}});
		operation.responses.emplace(403, ResponseData{
			.description = "The session role is insufficient",
			.content_type = "application/json",
			.schema_name = "ErrorResponse",
			.binary = false,
			.attachment = false,
			.filename = {},
			.example_json = {}});
	}
	operation.responses.emplace(500, ResponseData{
		.description = "An unexpected backend error occurred",
		.content_type = "application/json",
		.schema_name = "ErrorResponse",
		.binary = false,
		.attachment = false,
		.filename = {},
		.example_json = {}});
	const auto [position, inserted] = impl_->operations.emplace(
		operation_key(method, path), std::move(operation));
	if (!inserted)
		throw std::invalid_argument("duplicate OpenAPI operation: " +
			method_name(method) + " " + std::string(path));
	(void)position;
}

void DocumentedApiRegistry::add_parameter(Verb method,
	std::string_view path, std::string_view location, std::string_view name,
	std::string_view type, bool required, std::string_view description,
	std::vector<std::string> allowed_values, std::string_view example)
{
	if (name.empty() || type.empty() || description.empty())
		throw std::invalid_argument("incomplete OpenAPI parameter metadata");
	auto &operation = impl_->operation(method, path);
	if (std::ranges::any_of(operation.parameters, [&](const auto &parameter) {
		return parameter.name == name && parameter.location == location;
	}))
		throw std::invalid_argument("duplicate OpenAPI parameter: " +
			std::string(name));
	operation.parameters.push_back({std::string(name), std::string(location),
		std::string(type), required, std::string(description),
		std::move(allowed_values), std::string(example)});
}

void DocumentedApiRegistry::add_query_parameter(Verb method,
	std::string_view path, std::string_view name, std::string_view type,
	bool required, std::string_view description,
	std::vector<std::string> allowed_values, std::string_view example)
{
	add_parameter(method, path, "query", name, type, required,
		description, std::move(allowed_values), example);
}

void DocumentedApiRegistry::add_header_parameter(Verb method,
	std::string_view path, std::string_view name, std::string_view type,
	bool required, std::string_view description, std::string_view example)
{
	add_parameter(method, path, "header", name, type, required,
		description, {}, example);
}

void DocumentedApiRegistry::add_path_parameter(Verb method,
	std::string_view path, std::string_view name, std::string_view type,
	std::string_view description, std::string_view example)
{
	add_parameter(method, path, "path", name, type, true,
		description, {}, example);
}

void DocumentedApiRegistry::set_json_request(Verb method,
	std::string_view path, std::string_view schema_name,
	std::string schema_json, std::string_view description, bool required,
	std::string_view example_json)
{
	impl_->add_schema(schema_name, schema_json);
	auto &operation = impl_->operation(method, path);
	if (operation.request)
		throw std::invalid_argument("duplicate OpenAPI request body");
	operation.request = BodyData{std::string(description), required,
		{"application/json"}, std::string(schema_name), false,
		std::string(example_json)};
}

void DocumentedApiRegistry::set_json_response(Verb method,
	std::string_view path, unsigned status, std::string_view schema_name,
	std::string schema_json, std::string_view description,
	std::string_view example_json)
{
	impl_->add_schema(schema_name, schema_json);
	auto &responses = impl_->operation(method, path).responses;
	responses[status] = ResponseData{std::string(description),
		"application/json", std::string(schema_name), false, false, {},
		std::string(example_json)};
}

void DocumentedApiRegistry::add_json_object_response(Verb method,
	std::string_view path, unsigned status, std::string_view description,
	std::string_view example_json)
{
	auto &responses = impl_->operation(method, path).responses;
	responses[status] = ResponseData{std::string(description),
		"application/json", "JsonObject", false, false, {},
		std::string(example_json)};
}

void DocumentedApiRegistry::add_empty_response(Verb method,
	std::string_view path, unsigned status, std::string_view description)
{
	impl_->operation(method, path).responses[status] =
		ResponseData{std::string(description), std::nullopt, std::nullopt,
			false, false, {}, {}};
}

void DocumentedApiRegistry::add_text_response(Verb method,
	std::string_view path, unsigned status, std::string_view content_type,
	std::string_view description, std::string_view example)
{
	if (content_type.empty())
		throw std::invalid_argument("text response needs a content type");
	impl_->operation(method, path).responses[status] = ResponseData{
		std::string(description), std::string(content_type), std::nullopt,
		false, false, {}, std::string(example)};
}

void DocumentedApiRegistry::add_error_response(Verb method,
	std::string_view path, unsigned status, std::string_view description,
	std::string_view example_message)
{
	std::string example;
	if (!example_message.empty()) {
		Json value;
		value["error"] = example_message;
		example = dump_json(value);
	}
	impl_->operation(method, path).responses[status] = ResponseData{
		std::string(description), "application/json", "ErrorResponse",
		false, false, {}, std::move(example)};
}

void DocumentedApiRegistry::add_binary_request(Verb method,
	std::string_view path, std::vector<std::string> content_types,
	std::string_view description, bool required)
{
	if (content_types.empty())
		throw std::invalid_argument("binary request needs a content type");
	auto &operation = impl_->operation(method, path);
	if (operation.request)
		throw std::invalid_argument("duplicate OpenAPI request body");
	operation.request = BodyData{std::string(description), required,
		std::move(content_types), std::nullopt, true, {}};
}

void DocumentedApiRegistry::add_binary_response(Verb method,
	std::string_view path, unsigned status, std::string_view content_type,
	std::string_view description, std::string_view filename, bool attachment)
{
	impl_->operation(method, path).responses[status] = ResponseData{
		std::string(description), std::string(content_type), std::nullopt,
		true, attachment, std::string(filename), {}};
}

bool DocumentedApiRegistry::contains(Verb method, std::string_view path) const
{
	return impl_->operations.contains(operation_key(method, path));
}

std::size_t DocumentedApiRegistry::operation_count() const
{
	return impl_->operations.size();
}

void DocumentedApiRegistry::validate() const
{
	std::set<std::string> operation_ids;
	for (const auto &[key, operation] : impl_->operations) {
		(void)key;
		if (operation.summary.empty() || operation.tag.empty() ||
		    operation.operation_id.empty())
			throw std::runtime_error("incomplete OpenAPI operation: " +
				method_name(operation.method) + " " + operation.path);
		if (!operation_ids.insert(operation.operation_id).second)
			throw std::runtime_error("duplicate OpenAPI operationId: " +
				operation.operation_id);
		if (std::ranges::none_of(operation.responses,
			[](const auto &response) {
				return response.first >= 200 && response.first < 300;
			}))
			throw std::runtime_error("OpenAPI operation has no success response: " +
				method_name(operation.method) + " " + operation.path);
		for (const auto &[status, response] : operation.responses) {
			(void)status;
			if (response.description.empty())
				throw std::runtime_error("OpenAPI response has no description");
			if (response.schema_name &&
			    !impl_->schemas.contains(*response.schema_name))
				throw std::runtime_error("unresolved OpenAPI response schema: " +
					*response.schema_name);
		}
		if (operation.request && operation.request->schema_name &&
		    !impl_->schemas.contains(*operation.request->schema_name))
			throw std::runtime_error("unresolved OpenAPI request schema: " +
				*operation.request->schema_name);
	}

	const auto validate_refs = [&](const auto &self, const Json &value) -> void {
		if (const auto *object = value.get_if<Json::object_t>()) {
			if (const auto found = object->find("$ref");
			    found != object->end() && found->second.is_string()) {
				constexpr std::string_view prefix =
					"#/components/schemas/";
				const auto &reference = found->second.get<std::string>();
				if (!reference.starts_with(prefix) ||
				    !impl_->schemas.contains(reference.substr(prefix.size())))
					throw std::runtime_error(
						"unresolved OpenAPI $ref: " + reference);
			}
			for (const auto &[name, child] : *object) {
				(void)name;
				self(self, child);
			}
		} else if (const auto *array = value.get_if<Json::array_t>()) {
			for (const auto &child : *array) self(self, child);
		}
	};
	for (const auto &[name, schema] : impl_->schemas) {
		(void)name;
		validate_refs(validate_refs, schema);
	}
}

std::string DocumentedApiRegistry::yaml() const
{
	validate();
	Json document;
	document["openapi"] = "3.1.0";
	document["jsonSchemaDialect"] =
		"https://json-schema.org/draft/2020-12/schema";
	document["info"]["title"] = "MSAP1 REST API";
	document["info"]["version"] = "1.0.0";
	document["info"]["description"] =
		"Build-time contract for every external MSAP1 APU HTTP operation.";
	Json server;
	server["url"] = "/";
	document["servers"] = json_array({std::move(server)});

	for (const auto &[key, operation] : impl_->operations) {
		(void)key;
		Json item;
		item["summary"] = operation.summary;
		item["operationId"] = operation.operation_id;
		item["tags"] = string_array({operation.tag});
		if (operation.minimum_role) {
			Json requirement;
			requirement["sessionCookie"] = json_array({});
			item["security"] = json_array({std::move(requirement)});
			item["x-msap1-minimum-role"] =
				openapi_role_name(*operation.minimum_role);
		} else {
			item["security"] = json_array({});
			item["x-msap1-minimum-role"] = "public";
		}

		if (!operation.parameters.empty()) {
			std::vector<Json> parameters;
			for (const auto &parameter : operation.parameters) {
				Json value;
				value["name"] = parameter.name;
				value["in"] = parameter.location;
				value["required"] = parameter.required;
				value["description"] = parameter.description;
				value["schema"] = parameter_schema(parameter);
				if (!parameter.example.empty())
					value["example"] = parameter.example;
				parameters.push_back(std::move(value));
			}
			item["parameters"] = json_array(std::move(parameters));
		}

		if (operation.request) {
			Json request;
			request["description"] = operation.request->description;
			request["required"] = operation.request->required;
			for (const auto &content_type : operation.request->content_types) {
				auto &media = request["content"][content_type];
				media["schema"] = operation.request->binary
					? binary_schema()
					: schema_reference(*operation.request->schema_name);
				if (!operation.request->example_json.empty())
					media["example"] = parse_json(
						operation.request->example_json,
						"OpenAPI request example");
			}
			item["requestBody"] = std::move(request);
		}

		for (const auto &[status, response] : operation.responses) {
			auto &value = item["responses"][std::to_string(status)];
			value["description"] = response.description;
			if (response.content_type) {
				auto &media = value["content"][*response.content_type];
				media["schema"] = response.binary
					? binary_schema()
					: response.schema_name
						? schema_reference(*response.schema_name)
						: string_schema();
				if (!response.example_json.empty()) {
					if (response.schema_name)
						media["example"] = parse_json(response.example_json,
							"OpenAPI response example");
					else
						media["example"] = response.example_json;
				}
			}
			if (response.attachment) {
				auto &header = value["headers"]["Content-Disposition"];
				header["description"] = "Attachment filename";
				header["schema"]["type"] = "string";
				if (!response.filename.empty())
					header["example"] = "attachment; filename=\"" +
						response.filename + "\"";
			}
		}
		document["paths"][operation.path][method_name(operation.method)] =
			std::move(item);
	}

	for (const auto &[name, schema] : impl_->schemas)
		document["components"]["schemas"][name] = schema;
	auto &cookie = document["components"]["securitySchemes"]["sessionCookie"];
	cookie["type"] = "apiKey";
	cookie["in"] = "cookie";
	cookie["name"] = "session";
	cookie["description"] =
		"HTTP-only session cookie issued by POST /api/login.";

	auto output = glz::write_yaml(document).value_or(std::string{});
	if (output.empty())
		throw std::runtime_error("Glaze could not serialize OpenAPI YAML");
	return output;
}

std::string generate_openapi_yaml()
{
	DocumentedApiRegistry registry;
	for (const auto &route : route_table)
		registry.add_operation(route.method, route.path, route.min_role,
			route.summary);
	add_external_operations(registry);
	constexpr std::size_t external_operation_count = 15;
	if (registry.operation_count() !=
	    route_table.size() + external_operation_count)
		throw std::runtime_error(
			"OpenAPI route coverage differs from the WebEngine contract");

	document_health_routes(registry);
	document_attribute_routes(registry);
	document_meter_routes(registry);
	document_power_quality_routes(registry);
	document_energy_routes(registry);
	document_adc_routes(registry);
	document_waveform_routes(registry);
	document_settings_routes(registry);
	document_developer_routes(registry);
	document_database_routes(registry);
	document_history_routes(registry);
	document_mqtt_routes(registry);
	document_data_logging_routes(registry);
	document_documentation_routes(registry);

	registry.validate();
	return registry.yaml();
}

} // namespace msap1::web::api
