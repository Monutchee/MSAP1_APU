#include "audit/health_audit.hpp"

#include <array>
#include <chrono>
#include <string>

namespace msap1::service_manager::daemon {

using namespace std::chrono_literals;

HealthAuditor::HealthAuditor(boost::asio::io_context &context,
			     mnc::ServiceManager &manager,
			     const mnc::logging::Logger &logger)
	: manager_(manager), logger_(logger),
	  timer_(context.get_executor())
{
}

void HealthAuditor::start()
{
	schedule();
}

void HealthAuditor::schedule()
{
	timer_.expires_after(5s);
	timer_.async_wait([this](const boost::system::error_code &error) {
		if (error)
			return;
		run_once();
		schedule();
	});
}

void HealthAuditor::run_once()
{
	bool degraded = false;
	for (const auto &status : manager_.statuses()) {
		if (status.active_state == "active" &&
		    !status.permanently_failed)
			continue;
		degraded = true;
		if (status.permanently_failed) {
			const std::array fields{
				mnc::logging::Field{"MNC_SERVICE", status.name},
				mnc::logging::Field{
					"MNC_RESTART_COUNT",
					std::to_string(status.restart_count)}};
			(void)logger_.write(mnc::logging::Priority::critical,
				status.name + " exhausted its restart policy",
				"managed_service_failed", fields);
		}
	}
	degraded_ = degraded;
}

} // namespace msap1::service_manager::daemon
