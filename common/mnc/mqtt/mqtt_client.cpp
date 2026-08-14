#include "mnc/mqtt/mqtt_client.hpp"

#include <stdexcept>

namespace mnc::mqtt {

std::string server_uri(const ConnectionOptions &options)
{
	if (options.host.empty() || options.port == 0)
		throw std::invalid_argument("MQTT host and port are required");

	std::string scheme;
	switch (options.transport) {
	case Transport::mqtt: scheme = "mqtt"; break;
	case Transport::mqtts: scheme = "mqtts"; break;
	case Transport::ws: scheme = "ws"; break;
	case Transport::wss: scheme = "wss"; break;
	}
	/* URI authorities require brackets around a literal IPv6 address.  The
	 * stored setting remains the ordinary address without URI punctuation. */
	const auto host = options.host.find(':') != std::string::npos &&
		!(options.host.starts_with('[') && options.host.ends_with(']'))
		? "[" + options.host + "]" : options.host;
	auto result = scheme + "://" + host + ":" +
		std::to_string(options.port);
	if (options.transport == Transport::ws || options.transport == Transport::wss)
		result += options.websocket_path.empty() ? "/mqtt" : options.websocket_path;
	return result;
}

} // namespace mnc::mqtt
