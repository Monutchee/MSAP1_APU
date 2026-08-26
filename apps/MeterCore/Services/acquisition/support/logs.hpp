#pragma once

/**
 * @file logs.hpp
 * @brief Per-subsystem journald loggers and logging helpers shared by every
 *        module of the acquisition daemon.
 *
 * Each functional area logs under its own module name so `mnc log
 * --component fpga-acquisition --module <name>` can isolate one subsystem.
 */

#include "msap1/meter/meter_health.hpp"
#include "mnc/logging/logging.hpp"

#include <cerrno>
#include <cstring>
#include <initializer_list>
#include <source_location>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace msap1::acquisition::daemon {

inline const mnc::logging::Logger lifecycle_log{"fpga-acquisition", "lifecycle"};
inline const mnc::logging::Logger dma_log{"fpga-acquisition", "dma"};
inline const mnc::logging::Logger rpmsg_log{"fpga-acquisition", "rpmsg"};
inline const mnc::logging::Logger config_log{"fpga-acquisition", "adc-config"};
inline const mnc::logging::Logger health_log{"fpga-acquisition", "health"};
inline const mnc::logging::Logger aggregation_log{"fpga-acquisition",
						  "rpu-aggregation"};
inline const mnc::logging::Logger waveform_log{"fpga-acquisition", "waveform"};

/**
 * @brief Write one structured journal entry.
 *
 * @param logger   Subsystem logger selecting the MNC_MODULE field.
 * @param priority Journald priority of the entry.
 * @param message  Human-readable message text.
 * @param event    Stable machine-readable event identifier (MNC_EVENT).
 * @param fields   Additional structured journald fields.
 * @param source   Call site recorded in the journal entry.
 */
inline void log_message(
	const mnc::logging::Logger &logger, mnc::logging::Priority priority,
	std::string message, std::string_view event,
	std::initializer_list<mnc::logging::Field> fields = {},
	const std::source_location &source = std::source_location::current())
{
	(void)logger.write(priority, message, event,
			   std::span<const mnc::logging::Field>(
				   fields.begin(), fields.size()),
			   source);
}

/** @brief Join health reason codes into one comma-separated journal field. */
inline std::string health_reason_codes(
	const std::vector<msap1::HealthReason> &reasons)
{
	std::string result;
	for (const auto &reason : reasons) {
		if (!result.empty())
			result += ',';
		result += reason.code;
	}
	return result;
}

/** @brief Join health reason messages into one human-readable sentence. */
inline std::string health_reason_messages(
	const std::vector<msap1::HealthReason> &reasons)
{
	std::string result;
	for (const auto &reason : reasons) {
		if (!result.empty())
			result += "; ";
		result += reason.message;
	}
	return result;
}

/** @brief Throw a std::runtime_error naming @p operation and current errno. */
[[noreturn]] inline void throw_errno(const std::string &operation)
{
	throw std::runtime_error(operation + ": " + std::strerror(errno));
}

} // namespace msap1::acquisition::daemon
