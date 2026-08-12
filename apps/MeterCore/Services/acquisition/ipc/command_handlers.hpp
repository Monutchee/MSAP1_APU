#pragma once

/**
 * @file command_handlers.hpp
 * @brief Binds the typed acquisition command vocabulary to the coordinator.
 */

#include "msap1/acquisition/ipc/acquisition_ipc.hpp"

namespace msap1::acquisition::daemon {

class CaptureCoordinator;

/**
 * @brief Register a handler for every command in AcquisitionCommandList.
 *
 * Each registration pairs a command with its failure status (returned when
 * the handler throws) and a thin lambda calling the coordinator's public
 * API. Handlers run on the acquisition thread via IpcChannel::drain(), so
 * they may touch pipeline state freely.
 *
 * @param registry    The daemon's command registry.
 * @param coordinator Must outlive the registry (both live in the
 *                    coordinator, which registers itself).
 */
void register_acquisition_commands(msap1::AcquisitionCommandRegistry &registry,
				   CaptureCoordinator &coordinator);

} // namespace msap1::acquisition::daemon
