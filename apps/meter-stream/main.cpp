#include "meter_stream_service.hpp"

int main()
{
	try {
		msap1::meter_stream::daemon::MeterStreamService service;
		return service.execute();
	} catch (...) {
		return 1;
	}
}
