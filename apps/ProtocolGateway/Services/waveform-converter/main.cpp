#include "waveform_conversion_service.hpp"

int main()
{
	try {
		msap1::waveform::daemon::WaveformConversionService service;
		return service.execute();
	} catch (...) {
		return 1;
	}
}
