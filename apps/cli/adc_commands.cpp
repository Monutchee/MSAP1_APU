#include "cli.hpp"

#include "msap1/acquisition_ipc.hpp"

#include <cstdint>
#include <ostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace msap1::cli {
namespace {

void require_daemon_ok(const AcquisitionResponse &response)
{
	if (response.status != AcquisitionStatus::ok)
		throw std::runtime_error("acquisition daemon request failed (status " +
			std::to_string(static_cast<std::uint32_t>(response.status)) + ")");
}

int run_control(const Options &options, std::ostream &output,
		AcquisitionCommand command)
{
	AcquisitionClient client(options.socket_path);
	const auto response = client.request(command, options.timeout_ms);
	require_daemon_ok(response);
	output << "FPGA acquisition "
	       << (response.running != 0u ? "running" : "stopped") << '\n';
	return 0;
}

} // namespace

void register_adc_commands(Application &application)
{
	Command adc("adc", "Control ADC capture and FPGA acquisition");
	adc.add_subcommand(Command("start", "Start ADC capture and meter acquisition",
		[](const Options &options, std::ostream &output) {
			return run_control(options, output, AcquisitionCommand::start);
		}));
	adc.add_subcommand(Command("stop", "Stop ADC capture and meter acquisition",
		[](const Options &options, std::ostream &output) {
			return run_control(options, output, AcquisitionCommand::stop);
		}));
	application.add_command(std::move(adc));
}

} // namespace msap1::cli
