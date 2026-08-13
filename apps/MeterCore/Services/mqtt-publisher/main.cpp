#include "mqtt_publisher_service.hpp"

#include <exception>

int main()
{
	try {
		msap1::mqtt::daemon::MqttPublisherService service;
		return service.execute();
	} catch (...) {
		return 1;
	}
}
