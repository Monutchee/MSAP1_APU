#pragma once

#include "mnc/MeterDataProvider/attributes/meter_attribute.hpp"

#include <glaze/glaze.hpp>

#include <algorithm>
#include <cstdint>
#include <cctype>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace msap1::settings {

enum class MqttTransport : std::uint8_t { mqtt, mqtts, ws, wss };

struct MqttConnectionSettings {
	MqttTransport transport = MqttTransport::mqtt;
	std::string broker_host;
	std::uint16_t broker_port = 1883;
	std::string websocket_path = "/mqtt";
	std::string client_id = "msap1";
	std::string username;
	std::uint32_t keep_alive_seconds = 30;
	std::uint32_t connect_timeout_seconds = 10;
	std::uint32_t reconnect_min_seconds = 1;
	std::uint32_t reconnect_max_seconds = 60;
	bool operator==(const MqttConnectionSettings &) const = default;
};

struct MqttTlsSettings {
	bool use_system_ca = true;
	bool verify_peer = true;
	bool verify_hostname = true;
	bool use_client_certificate = false;
	bool operator==(const MqttTlsSettings &) const = default;
};

struct MqttPublicationSettings {
	std::string id;
	bool enabled = true;
	std::string topic;
	std::string period = "basic";
	std::uint32_t interval_ms = 1000;
	std::uint8_t qos = 1;
	bool retain = false;
	std::vector<std::string> attributes;
	bool operator==(const MqttPublicationSettings &) const = default;
};

struct MqttSettings {
	bool enabled = false;
	MqttConnectionSettings connection;
	MqttTlsSettings tls;
	std::vector<MqttPublicationSettings> publications;
	bool operator==(const MqttSettings &) const = default;

	void validate() const
	{
		const auto contains_control = [](std::string_view value) {
			return std::ranges::any_of(value, [](unsigned char character) {
				return std::iscntrl(character) != 0;
			});
		};
		const auto secure_transport =
			connection.transport == MqttTransport::mqtts ||
			connection.transport == MqttTransport::wss;

		if (connection.broker_host.find_first_of("\r\n\t ") !=
		    std::string::npos)
			throw std::runtime_error("MQTT broker host contains whitespace");
		if (contains_control(connection.broker_host) ||
		    contains_control(connection.client_id) ||
		    contains_control(connection.username))
			throw std::runtime_error("MQTT connection text contains control characters");
		if (enabled && connection.broker_host.empty())
			throw std::runtime_error("MQTT broker host is required");
		if (enabled && connection.client_id.empty())
			throw std::runtime_error("MQTT client ID is required");
		if (connection.client_id.size() > 128)
			throw std::runtime_error("MQTT client ID is too long");
		if (connection.broker_port == 0)
			throw std::runtime_error("MQTT broker port must be nonzero");
		if (connection.keep_alive_seconds == 0 ||
		    connection.connect_timeout_seconds == 0)
			throw std::runtime_error("MQTT timeouts must be nonzero");
		if (connection.reconnect_min_seconds == 0 ||
		    connection.reconnect_min_seconds > connection.reconnect_max_seconds)
			throw std::runtime_error("invalid MQTT reconnect range");
		if ((connection.transport == MqttTransport::ws ||
		     connection.transport == MqttTransport::wss) &&
		    (connection.websocket_path.empty() ||
		     connection.websocket_path.front() != '/'))
			throw std::runtime_error("MQTT WebSocket path must begin with '/'");
		if (contains_control(connection.websocket_path) ||
		    connection.websocket_path.find_first_of(" #") != std::string::npos)
			throw std::runtime_error("invalid MQTT WebSocket path");
		if (secure_transport && tls.verify_hostname && !tls.verify_peer)
			throw std::runtime_error(
				"MQTT hostname verification requires peer verification");
		/*
		 * TLS preferences may remain stored while a clear-text transport is
		 * selected.  They are consumed only by MQTTS/WSS, which lets an
		 * administrator change transports without losing installed assets.
		 */
		if (publications.size() > 64)
			throw std::runtime_error("too many MQTT publications");

		std::unordered_set<std::string> ids;
		for (const auto &publication : publications) {
			if (publication.id.empty() || contains_control(publication.id) ||
			    !ids.insert(publication.id).second)
				throw std::runtime_error("MQTT publication IDs must be unique");
			if (publication.topic.empty() ||
			    contains_control(publication.topic) ||
			    publication.topic.find_first_of("+#") != std::string::npos)
				throw std::runtime_error("invalid MQTT publication topic");
			if (publication.interval_ms < 100)
				throw std::runtime_error("MQTT interval must be at least 100 ms");
			if (publication.qos > 2)
				throw std::runtime_error("MQTT QoS must be 0, 1, or 2");
			if (publication.enabled && publication.attributes.empty())
				throw std::runtime_error(
					"enabled MQTT publication has no selected attributes");
			if (publication.period != "basic" &&
			    publication.period != "cycles150_180" &&
			    publication.period != "min10" &&
			    publication.period != "hour2")
				throw std::runtime_error("unknown MQTT measurement period");
			for (const auto &attribute : publication.attributes)
				if (!mnc::meter::find_attribute(attribute))
					throw std::runtime_error(
						"unknown MQTT meter attribute: " + attribute);
		}
	}
};

} // namespace msap1::settings

template<>
struct glz::meta<msap1::settings::MqttTransport> {
	static constexpr auto value = glz::enumerate(
		"mqtt", msap1::settings::MqttTransport::mqtt,
		"mqtts", msap1::settings::MqttTransport::mqtts,
		"ws", msap1::settings::MqttTransport::ws,
		"wss", msap1::settings::MqttTransport::wss);
};
