#pragma once

#include <string_view>

namespace msap1::web {

// Minimal sd_notify-compatible client without a libsystemd build dependency.
// Calls are harmless when NOTIFY_SOCKET is not present (for local development).
class SystemdNotifier {
public:
	bool send(std::string_view state) const noexcept;
	bool ready(std::string_view status) const noexcept;
	bool watchdog(std::string_view status) const noexcept;
	bool stopping(std::string_view status) const noexcept;
};

} // namespace msap1::web
