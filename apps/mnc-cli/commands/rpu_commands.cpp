#include "core/cli.hpp"
#include "core/result_output.hpp"

#include "msap1/acquisition/ipc/acquisition_ipc.hpp"
#include "msap1/meter/meter_health.hpp"
#include "msap1/system/rpu_runtime_status.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <ostream>
#include <string>
#include <vector>

namespace msap1::cli {
namespace {

struct AggregationHealthResult {
	bool available = false;
	bool probe_pending = false;
	bool healthy = false;
	bool authoritative = false;
	std::uint32_t age_ms = acquisition_age_unavailable;
	std::uint32_t probe_failures = 0;
	std::string rpmsg_device;
	std::uint32_t health_flags = 0;
	std::uint32_t frames_received = 0;
	std::uint32_t frames_valid = 0;
	std::uint32_t frames_invalid = 0;
	std::uint32_t sequence_gaps = 0;
	std::uint32_t ring_overflows = 0;
	std::uint32_t fifo_errors = 0;
	std::uint32_t records_queued = 0;
	std::uint32_t records_emitted = 0;
	std::uint32_t output_errors = 0;
	std::uint32_t output_drops = 0;
	std::uint32_t basic_completed = 0;
	std::uint32_t aggregate_completed = 0;
	std::uint32_t ten_minute_completed = 0;
	std::uint32_t two_hour_completed = 0;
	std::vector<HealthReason> degraded_reasons;
};

struct RpuResult {
	bool remoteproc_healthy = false;
	bool rpmsg_available = false;
	bool acquisition_connected = false;
	std::string acquisition_error;
	RpuRuntimeStatus runtime;
	AggregationHealthResult aggregation;
};

bool aggregation_flag(const msap1_aggregation_health_payload &health,
		      std::uint32_t flag)
{
	return (health.health_flags & flag) != 0u;
}

AggregationHealthResult aggregation_result(const InfoResponse &response)
{
	AggregationHealthResult result{
		.available = response.has_aggregation_health,
		.probe_pending = response.aggregation_health_probe_pending,
		.age_ms = response.aggregation_health_age_ms,
		.probe_failures = response.aggregation_health_probe_failures,
		.rpmsg_device = response.aggregation_rpmsg_device,
		.degraded_reasons = {},
	};
	if (!response.has_aggregation_health)
		return result;

	const auto health = response.rpu_aggregation_health.value();
	result.health_flags = health.health_flags;
	result.authoritative = aggregation_flag(
		health, MSAP1_AGGREGATION_HEALTH_AUTHORITATIVE);
	result.degraded_reasons =
		evaluate_rpu_aggregation_health_reasons(health);
	result.healthy = result.degraded_reasons.empty();
	result.frames_received = health.frames_received;
	result.frames_valid = health.frames_valid;
	result.frames_invalid = health.frames_invalid;
	result.sequence_gaps = health.sequence_gaps;
	result.ring_overflows = health.ring_overflows;
	result.fifo_errors = health.fifo_errors;
	result.records_queued = health.records_queued;
	result.records_emitted = health.records_emitted;
	result.output_errors = health.output_errors;
	result.output_drops = health.output_drops;
	result.basic_completed = health.basic_completed;
	result.aggregate_completed = health.aggregate_completed;
	result.ten_minute_completed = health.ten_minute_completed;
	result.two_hour_completed = health.two_hour_completed;
	return result;
}

RpuResult collect_rpu_result(const Options &options)
{
	RpuResult result;
	result.runtime = RpuRuntimeInspector{}.inspect();
	result.remoteproc_healthy =
		!result.runtime.remote_processors.empty() &&
		std::ranges::all_of(result.runtime.remote_processors,
			[](const auto &processor) {
				return processor.state == "running";
			});
	result.rpmsg_available = !result.runtime.rpmsg_devices.empty() ||
		!result.runtime.rpmsg_device_nodes.empty();

	try {
		AcquisitionClient client(options.socket_path);
		const auto response =
			client.request(HealthRequest{}, options.timeout_ms);
		result.acquisition_connected =
			response.status == AcquisitionStatus::ok;
		if (result.acquisition_connected)
			result.aggregation = aggregation_result(response);
		else
			result.acquisition_error = "acquisition health request failed";
	} catch (const std::exception &error) {
		result.acquisition_error = error.what();
	}
	return result;
}

class RpuTextGenerator final : public ResultGenerator<RpuResult> {
public:
	int write(const RpuResult &result, std::ostream &output) const override
	{
		output << "MSAP1 RPU status\n"
		       << "  Remoteproc:           "
		       << (result.remoteproc_healthy ? "healthy" : "degraded")
		       << '\n';
		if (result.runtime.remote_processors.empty())
			output << "    no remote processors discovered\n";
		for (const auto &processor : result.runtime.remote_processors) {
			output << "    " << processor.identifier << "  "
			       << (processor.name.empty() ? "unnamed" : processor.name)
			       << "  state="
			       << (processor.state.empty() ? "unknown" : processor.state);
			if (!processor.firmware.empty())
				output << "  firmware=" << processor.firmware;
			output << '\n';
		}

		output << "  RPMsg:                "
		       << (result.rpmsg_available ? "available" : "unavailable")
		       << '\n';
		for (const auto &device : result.runtime.rpmsg_devices) {
			output << "    " << device.identifier;
			if (!device.name.empty())
				output << "  name=" << device.name;
			if (!device.driver.empty())
				output << "  driver=" << device.driver;
			output << '\n';
		}
		for (const auto &node : result.runtime.rpmsg_device_nodes)
			output << "    node=" << node << '\n';

		output << "  Acquisition cache:    "
		       << (result.acquisition_connected ? "connected" : "unavailable")
		       << '\n';
		if (!result.acquisition_error.empty())
			output << "    " << result.acquisition_error << '\n';

		const auto &aggregation = result.aggregation;
		output << "  R5C1 aggregation:     ";
		if (!aggregation.available) {
			output << "unavailable";
			if (aggregation.probe_pending)
				output << " (probe pending)";
			output << '\n';
		} else {
			output << (aggregation.healthy ? "healthy" : "degraded")
			       << " ("
			       << (aggregation.authoritative ? "authoritative" : "shadow")
			       << ")\n"
			       << "    endpoint=" << aggregation.rpmsg_device;
			if (aggregation.age_ms != acquisition_age_unavailable)
				output << "  age=" << aggregation.age_ms << " ms";
			output << "  flags=0x" << std::hex << std::setw(8)
			       << std::setfill('0') << aggregation.health_flags
			       << std::dec << std::setfill(' ') << '\n'
			       << "    input frames=" << aggregation.frames_received
			       << " valid=" << aggregation.frames_valid
			       << " invalid=" << aggregation.frames_invalid
			       << " gaps=" << aggregation.sequence_gaps << '\n'
			       << "    records queued=" << aggregation.records_queued
			       << " emitted=" << aggregation.records_emitted
			       << " drops=" << aggregation.output_drops << '\n'
			       << "    completed basic=" << aggregation.basic_completed
			       << " aggregate=" << aggregation.aggregate_completed
			       << " 10-minute=" << aggregation.ten_minute_completed
			       << " 2-hour=" << aggregation.two_hour_completed << '\n';
			for (const auto &reason : aggregation.degraded_reasons)
				output << "    - " << reason.message << '\n';
		}
		return result.remoteproc_healthy && result.rpmsg_available ? 0 : 1;
	}
};

class RpuJsonGenerator final : public ResultGenerator<RpuResult> {
public:
	int write(const RpuResult &result, std::ostream &output) const override
	{
		write_json_success(output, result);
		return 0;
	}
};

int show_rpu_status(const Options &options, std::ostream &output)
{
	const auto result = collect_rpu_result(options);
	return render_result(options, result, output,
			     RpuTextGenerator{}, RpuJsonGenerator{});
}

} // namespace

void register_rpu_command(Application &application)
{
	application.add_command(Command(
		"rpu", "Show remoteproc, RPMsg, and R5C1 aggregation status",
		show_rpu_status,
		{
			.access = AccessLevel::diagnostic,
			.side_effect = SideEffect::none,
			.supports_text = true,
			.supports_json = true,
			.variants = {},
		}));
}

} // namespace msap1::cli
