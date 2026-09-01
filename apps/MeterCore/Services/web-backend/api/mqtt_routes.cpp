/** @file mqtt_routes.cpp MQTT configuration, capabilities, and credential APIs. */

#include "openapi.hpp"
#include "response.hpp"
#include "routes.hpp"

#include "msap1/meter/MeterDataProvider/snapshot/acquisition_meter_snapshot_provider.hpp"
#include "msap1/mqtt/meter_publication_catalog.hpp"

#include <algorithm>
#include <exception>
#include <optional>
#include <string>
#include <string_view>

#include <glaze/glaze.hpp>

namespace msap1::web::api {
namespace {

struct CredentialStatusDto {
	bool password_configured = false;
	bool private_key_passphrase_configured = false;
	bool ca_configured = false;
	bool client_certificate_configured = false;
	bool client_key_configured = false;
};

struct MqttConfigurationDto {
	settings::MqttSettings settings;
	CredentialStatusDto credentials;
};

struct PasswordUpdateDto {
	std::string password;
};

CredentialStatusDto credential_status(AppContext &app)
{
	return {
		.password_configured = app.mqtt.secret_present("mqtt.password"),
		.private_key_passphrase_configured =
			app.mqtt.secret_present("mqtt.private_key_passphrase"),
		.ca_configured = app.mqtt.asset_present("ca"),
		.client_certificate_configured =
			app.mqtt.asset_present("client-certificate"),
		.client_key_configured = app.mqtt.asset_present("client-key"),
	};
}

MqttConfigurationDto configuration(AppContext &app)
{
	return {settings::SettingsCodec::decode(app.settings.active().json).mqtt,
		credential_status(app)};
}

webengine::Response upload(AppContext &app, std::string name,
	const webengine::FileUpload &file)
{
	try {
		app.mqtt.upload_asset(std::move(name), file.contents);
		return json_response(webengine::http::status::ok,
			credential_status(app));
	} catch (const std::exception &error) {
		return error_response(webengine::http::status::bad_request,
			error.what());
	}
}

webengine::Response remove_asset(AppContext &app, std::string name)
{
	try {
		app.mqtt.delete_asset(std::move(name));
		return json_response(webengine::http::status::ok,
			credential_status(app));
	} catch (const std::exception &error) {
		return error_response(webengine::http::status::conflict,
			error.what());
	}
}

std::optional<webengine::FileDownload> download(AppContext &app,
	std::string asset, std::string filename)
{
	if (!app.mqtt.asset_present(asset))
		return std::nullopt;
	return webengine::FileDownload{std::move(filename),
		"application/x-pem-file", app.mqtt.download_asset(std::move(asset))};
}

} // namespace

webengine::Response get_mqtt_capabilities(
	AppContext &, const webengine::RequestContext &)
{
	try {
		msap1::meter::AcquisitionMeterSnapshotProvider provider;
		return json_response(webengine::http::status::ok,
			mqtt::MeterPublicationCatalog::capabilities(provider));
	} catch (const std::exception &error) {
		return error_response(webengine::http::status::service_unavailable,
			error.what());
	}
}

webengine::Response get_mqtt_configuration(
	AppContext &app, const webengine::RequestContext &)
{
	try {
		return json_response(webengine::http::status::ok,
			configuration(app));
	} catch (const std::exception &error) {
		return error_response(webengine::http::status::service_unavailable,
			error.what());
	}
}

webengine::Response put_mqtt_configuration(
	AppContext &app, const webengine::RequestContext &context)
{
	try {
		settings::MqttSettings input;
		if (glz::read_json(input, context.request.body()))
			return error_response(webengine::http::status::bad_request,
				"invalid MQTT configuration JSON");
		input.validate();
		msap1::meter::AcquisitionMeterSnapshotProvider provider;
		const auto capabilities =
			mqtt::MeterPublicationCatalog::capabilities(provider);
		for (const auto &publication : input.publications) {
			if (!publication.enabled)
				continue;
			const auto period = std::ranges::find_if(capabilities,
				[&](const auto &candidate) {
					return candidate.id == publication.period;
				});
			if (period == capabilities.end())
				throw std::invalid_argument(
					"selected MQTT period is not available");
			for (const auto &attribute : publication.attributes) {
				const auto supported = std::ranges::find_if(
					period->attributes, [&](const auto &candidate) {
						return candidate.id == attribute;
					});
				if (supported == period->attributes.end())
					throw std::invalid_argument(
						"MQTT attribute is unavailable for selected period: " +
						attribute);
			}
		}
		const auto credentials = credential_status(app);
		const auto secure = input.connection.transport ==
			settings::MqttTransport::mqtts ||
			input.connection.transport == settings::MqttTransport::wss;
		if (secure && !input.tls.use_system_ca &&
		    !credentials.ca_configured)
			throw std::invalid_argument(
				"uploaded CA certificate is required");
		if (secure && input.tls.use_client_certificate &&
		    (!credentials.client_certificate_configured ||
		     !credentials.client_key_configured))
			throw std::invalid_argument(
				"client certificate and private key are required");
		(void)app.settings.update_and_save(
			[&](settings::ProductSettings &value) { value.mqtt = input; },
			120000);
		return json_response(webengine::http::status::ok,
			configuration(app));
	} catch (const std::invalid_argument &error) {
		return error_response(webengine::http::status::bad_request,
			error.what());
	} catch (const std::exception &error) {
		log_api_failure("/api/v1/mqtt/configuration", error);
		return error_response(webengine::http::status::conflict, error.what());
	}
}

webengine::Response get_mqtt_status(
	AppContext &app, const webengine::RequestContext &)
{
	/*
	 * A disabled publisher intentionally has no running service or socket.
	 * Check the persisted policy first so a normal disabled state never waits
	 * for an IPC timeout or reports the absent socket as a runtime failure.
	 */
	try {
		const auto active = settings::SettingsCodec::decode(
			app.settings.active().json);
		if (!active.mqtt.enabled)
			return json_response(webengine::http::status::ok,
				mqtt::MqttServiceStatus{});
	} catch (const std::exception &error) {
		return error_response(webengine::http::status::service_unavailable,
			error.what());
	}

	try {
		return json_response(webengine::http::status::ok, app.mqtt.status());
	} catch (const std::exception &error) {
		return error_response(webengine::http::status::service_unavailable,
			error.what());
	}
}

webengine::Response put_mqtt_password(
	AppContext &app, const webengine::RequestContext &context)
{
	try {
		PasswordUpdateDto value;
		if (glz::read_json(value, context.request.body()) || value.password.empty())
			return error_response(webengine::http::status::bad_request,
				"a nonempty password is required");
		app.mqtt.set_secret("mqtt.password", std::move(value.password));
		return json_response(webengine::http::status::ok,
			credential_status(app));
	} catch (const std::exception &error) {
		return error_response(webengine::http::status::bad_request, error.what());
	}
}

webengine::Response delete_mqtt_password(
	AppContext &app, const webengine::RequestContext &)
{
	try {
		app.mqtt.clear_secret("mqtt.password");
		return json_response(webengine::http::status::ok,
			credential_status(app));
	} catch (const std::exception &error) {
		return error_response(webengine::http::status::conflict, error.what());
	}
}

webengine::Response put_mqtt_private_key_passphrase(
	AppContext &app, const webengine::RequestContext &context)
{
	try {
		PasswordUpdateDto value;
		if (glz::read_json(value, context.request.body()) || value.password.empty())
			return error_response(webengine::http::status::bad_request,
				"a nonempty private-key passphrase is required");
		app.mqtt.set_secret("mqtt.private_key_passphrase",
			std::move(value.password));
		return json_response(webengine::http::status::ok,
			credential_status(app));
	} catch (const std::exception &error) {
		return error_response(webengine::http::status::bad_request,
			error.what());
	}
}

webengine::Response delete_mqtt_private_key_passphrase(
	AppContext &app, const webengine::RequestContext &)
{
	try {
		app.mqtt.clear_secret("mqtt.private_key_passphrase");
		return json_response(webengine::http::status::ok,
			credential_status(app));
	} catch (const std::exception &error) {
		return error_response(webengine::http::status::conflict, error.what());
	}
}

webengine::Response upload_mqtt_ca(AppContext &app,
	const webengine::RequestContext &, const webengine::FileUpload &file)
{
	return upload(app, "ca", file);
}
webengine::Response upload_mqtt_client_certificate(AppContext &app,
	const webengine::RequestContext &, const webengine::FileUpload &file)
{
	return upload(app, "client-certificate", file);
}
webengine::Response upload_mqtt_client_key(AppContext &app,
	const webengine::RequestContext &, const webengine::FileUpload &file)
{
	return upload(app, "client-key", file);
}
webengine::Response delete_mqtt_ca(AppContext &app,
	const webengine::RequestContext &)
{
	return remove_asset(app, "ca");
}
webengine::Response delete_mqtt_client_certificate(AppContext &app,
	const webengine::RequestContext &)
{
	return remove_asset(app, "client-certificate");
}
webengine::Response delete_mqtt_client_key(AppContext &app,
	const webengine::RequestContext &)
{
	return remove_asset(app, "client-key");
}
std::optional<webengine::FileDownload> download_mqtt_ca(AppContext &app,
	const webengine::RequestContext &)
{
	return download(app, "ca", "mqtt-ca.pem");
}
std::optional<webengine::FileDownload> download_mqtt_client_certificate(
	AppContext &app, const webengine::RequestContext &)
{
	return download(app, "client-certificate", "mqtt-client-certificate.pem");
}

void document_mqtt_routes(DocumentedApiRegistry &registry)
{
	using V = webengine::http::verb;
	constexpr auto capabilities = "/api/v1/mqtt/capabilities";
	registry.add_json_response<std::vector<mqtt::PublicationPeriodCapability>>(
		V::get, capabilities, 200, "MqttCapabilities",
		"MQTT-selectable periods and canonical attributes");
	registry.add_error_response(V::get, capabilities, 503,
		"Meter publication capabilities are unavailable");

	constexpr auto configuration_path = "/api/v1/mqtt/configuration";
	registry.add_json_response<MqttConfigurationDto>(V::get,
		configuration_path, 200, "MqttConfiguration",
		"Active MQTT policy and credential-presence flags");
	registry.add_error_response(V::get, configuration_path, 503,
		"MQTT settings are unavailable");
	registry.add_json_request<settings::MqttSettings>(V::put,
		configuration_path, "MqttSettings", "Complete MQTT policy");
	registry.add_json_response<MqttConfigurationDto>(V::put,
		configuration_path, 200, "MqttConfiguration",
		"Saved MQTT policy and credential-presence flags");
	registry.add_error_response(V::put, configuration_path, 400,
		"The MQTT policy is invalid or required credentials are absent");
	registry.add_error_response(V::put, configuration_path, 409,
		"The settings authority rejected the MQTT policy");

	constexpr auto status_path = "/api/v1/mqtt/status";
	registry.add_json_response<mqtt::MqttServiceStatus>(V::get, status_path,
		200, "MqttStatus", "Publisher connection and publication status");
	registry.add_error_response(V::get, status_path, 503,
		"MQTT policy or publisher status is unavailable");

	for (const auto path : {"/api/v1/mqtt/credentials/password",
		"/api/v1/mqtt/credentials/private-key-passphrase"}) {
		registry.add_json_request<PasswordUpdateDto>(V::put, path,
			"SecretUpdate", "Replacement secret value");
		registry.add_json_response<CredentialStatusDto>(V::put, path, 200,
			"MqttCredentialStatus", "Updated credential-presence flags");
		registry.add_error_response(V::put, path, 400,
			"A nonempty secret is required");
		registry.add_json_response<CredentialStatusDto>(V::delete_, path,
			200, "MqttCredentialStatus", "Updated credential-presence flags");
		registry.add_error_response(V::delete_, path, 409,
			"The settings authority could not remove the secret");
	}

	const std::vector<std::string> certificate_types{
		"application/octet-stream", "application/x-pem-file",
		"application/pkix-cert", "application/x-x509-ca-cert"};
	for (const std::string_view path : {"/api/v1/mqtt/tls/ca",
		"/api/v1/mqtt/tls/client-certificate",
		"/api/v1/mqtt/tls/client-key"}) {
		registry.add_header_parameter(V::put, path, "X-File-Name", "string",
			true, "Safe source filename", "credential.pem");
		registry.add_binary_request(V::put, path, certificate_types,
			"PEM or certificate payload, limited to 1 MiB");
		registry.add_json_response<CredentialStatusDto>(V::put, path, 200,
			"MqttCredentialStatus", "Updated credential-presence flags");
		registry.add_error_response(V::put, path, 400,
			"The credential asset could not be installed");
		registry.add_error_response(V::put, path, 413,
			"The upload exceeds 1 MiB");
		registry.add_error_response(V::put, path, 415,
			"The upload content type is unsupported");
		registry.add_json_response<CredentialStatusDto>(V::delete_, path,
			200, "MqttCredentialStatus", "Updated credential-presence flags");
		registry.add_error_response(V::delete_, path, 409,
			"The settings authority could not remove the asset");
	}
	for (const std::string_view path : {"/api/v1/mqtt/tls/ca",
		"/api/v1/mqtt/tls/client-certificate"}) {
		registry.add_binary_response(V::get, path, 200,
			"application/x-pem-file", "Installed public credential asset",
			path.ends_with("/ca") ? "mqtt-ca.pem" :
				"mqtt-client-certificate.pem");
		registry.add_error_response(V::get, path, 404,
			"The credential asset is not configured");
	}
}

} // namespace msap1::web::api
