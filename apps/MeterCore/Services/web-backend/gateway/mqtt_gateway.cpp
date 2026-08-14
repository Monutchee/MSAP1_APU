#include "gateway/mqtt_gateway.hpp"

namespace msap1::web {

mqtt::MqttServiceStatus MqttGateway::status(int timeout_ms) const
{
	return status_.status(timeout_ms);
}

void MqttGateway::set_secret(std::string name, std::string value,
	int timeout_ms) const
{
	settings_.set_secret(std::move(name), std::move(value), timeout_ms);
}

void MqttGateway::clear_secret(std::string name, int timeout_ms) const
{
	settings_.clear_secret(std::move(name), timeout_ms);
}

bool MqttGateway::secret_present(std::string name, int timeout_ms) const
{
	return settings_.secret_present(std::move(name), timeout_ms);
}

void MqttGateway::upload_asset(std::string name, std::string contents,
	int timeout_ms) const
{
	settings_.put_asset(std::move(name), std::move(contents), timeout_ms);
}

void MqttGateway::delete_asset(std::string name, int timeout_ms) const
{
	settings_.delete_asset(std::move(name), timeout_ms);
}

bool MqttGateway::asset_present(std::string name, int timeout_ms) const
{
	return settings_.asset_present(std::move(name), timeout_ms);
}

std::string MqttGateway::download_asset(std::string name, int timeout_ms) const
{
	return settings_.download_asset(std::move(name), timeout_ms);
}

} // namespace msap1::web
