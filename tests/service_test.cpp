#include "mnc/service/service.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <exception>
#include <iostream>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;

void require(bool condition, const std::string &message)
{
	if (!condition)
		throw std::runtime_error(message);
}

class ReloadingService final : public mnc::Service {
public:
	ReloadingService() : Service("test service", "service-test") {}

	~ReloadingService() override
	{
		if (reload_request_.joinable())
			reload_request_.join();
	}

	[[nodiscard]] bool reloaded() const noexcept { return reloaded_; }

private:
	void on_start() override
	{
		reload_request_ = std::thread([this] {
			std::this_thread::sleep_for(20ms);
			request_reload();
		});
	}

	void on_reload() override
	{
		/* Reproduce a normal reload operation clobbering errno after the
		 * lifecycle wait timed out with EAGAIN. */
		errno = ENOENT;
		reloaded_ = true;
		request_stop();
	}

	void on_stop() noexcept override
	{
		if (reload_request_.joinable())
			reload_request_.join();
	}

	[[nodiscard]] mnc::ServiceHealth health() const override
	{
		return {.healthy = true, .summary = "test healthy"};
	}

	std::thread reload_request_;
	std::atomic<bool> reloaded_{false};
};

} // namespace

int main()
{
	try {
		ReloadingService service;
		require(service.execute() == 0,
			"a successful internal reload must not become a signal error");
		require(service.reloaded(), "the requested reload did not run");
		std::cout << "service lifecycle tests passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "service lifecycle test failed: " << error.what()
			  << '\n';
		return 1;
	}
}
