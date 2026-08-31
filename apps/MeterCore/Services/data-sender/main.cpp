#include "data_sender_service.hpp"

int main()
{
	try {
		msap1::datalogger::daemon::DataSenderService service;
		return service.execute();
	} catch (...) {
		return 1;
	}
}
