#include "meter_historian_service.hpp"
int main() { try { msap1::history::daemon::MeterHistorianService service; return service.execute(); } catch (...) { return 1; } }
