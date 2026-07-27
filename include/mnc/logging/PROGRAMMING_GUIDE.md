# `mnc::logging` Programming Guide

`mnc::logging` provides best-effort structured logging to systemd-journald and
a cursor-based API for reading those entries. Journald is the authoritative log
store; applications must not create a parallel text log or logging database.

The public headers are:

```cpp
#include <mnc/logging/logging.hpp>        // Logger and Priority
#include <mnc/logging/journal_reader.hpp> // Query and JournalReader
```

Link an application or library to the CMake target:

```cmake
target_link_libraries(my_application PRIVATE mnc::logging)
```

## 1. Creating a Logger

A logger has two classifications:

- **Component** identifies the process or service.
- **Module** identifies an area inside that component.

Create loggers once and reuse them:

```cpp
#include <mnc/logging/logging.hpp>

namespace {

const mnc::logging::Logger lifecycle_log{
    "fpga-acquisition", "lifecycle"};
const mnc::logging::Logger dma_log{
    "fpga-acquisition", "dma"};
const mnc::logging::Logger rpmsg_log{
    "fpga-acquisition", "rpmsg"};

} // namespace
```

This produces journal fields such as:

```text
MNC_COMPONENT=fpga-acquisition
MNC_MODULE=dma
```

The component is required. Constructing a logger with an empty component throws
`std::invalid_argument`. The module is optional:

```cpp
const mnc::logging::Logger application_log{"my-application"};
```

Use stable, lowercase names. Do not place changing values such as a PID, device
path, username, or configuration generation in the component or module name.
Store those values in structured fields instead.

## 2. Writing Log Entries

`Logger::write()` is the canonical writer:

```cpp
bool write(
    mnc::logging::Priority priority,
    std::string_view message,
    std::string_view event = {},
    std::span<const mnc::logging::Field> fields = {},
    const std::source_location& source =
        std::source_location::current()) const noexcept;
```

### Basic entry

```cpp
(void)lifecycle_log.write(
    mnc::logging::Priority::notice,
    "FPGA acquisition service started",
    "service_started");
```

The entry contains:

```text
MESSAGE=FPGA acquisition service started
PRIORITY=5
MNC_COMPONENT=fpga-acquisition
MNC_MODULE=lifecycle
MNC_EVENT=service_started
CODE_FILE=<calling source file>
CODE_LINE=<calling source line>
CODE_FUNC=<calling function>
```

`MESSAGE` is for people. `MNC_EVENT` is a stable machine-readable identifier
for scripts, tests, and a future MCP server. Keep the event name stable even if
the human-readable message changes.

### Structured fields

Use an array when adding fields:

```cpp
#include <array>
#include <string>

const std::array<mnc::logging::Field, 2> fields{{
    {"MNC_DEVICE", device_path},
    {"MNC_CONFIGURATION_GENERATION",
     std::to_string(configuration_generation)},
}};

(void)dma_log.write(
    mnc::logging::Priority::info,
    "meter DMA device opened: " + device_path,
    "dma_opened",
    fields);
```

Custom journal field names:

- Must contain only uppercase `A-Z`, digits, and `_`.
- Must not be empty.
- Must not begin with `_`; that namespace belongs to journald.
- Should use the `MNC_` prefix for Monutchee application fields.

Invalid field names are ignored. The rest of the entry is still submitted.

Common correlation fields are:

```text
MNC_REQUEST_ID
MNC_CONFIGURATION_GENERATION
MNC_DEVICE
MNC_HTTP_ROUTE
MNC_USERNAME
```

Do not write passwords, session secrets, authentication tokens, private keys,
or other sensitive data to the journal.

### Priority levels

The priorities follow syslog/journald numbering:

| Priority | Typical use |
|---|---|
| `emergency` | The complete system is unusable. |
| `alert` | Immediate operator action is required. |
| `critical` | A service cannot perform its primary function. |
| `error` | An operation failed. |
| `warning` | Degraded or unexpected behavior that recovered or can continue. |
| `notice` | A significant normal state transition. |
| `info` | Routine lifecycle and operational information. |
| `debug` | Detailed diagnostic information. |

Lower numeric values are more severe. Prefer `notice` for start, stop, and
configuration transitions; use `info` for ordinary activity.

### Return value and failure behavior

`write()` returns `true` when journald accepted the entry and `false` when it
did not. It is `noexcept` and catches internal allocation or journald failures.

Most real-time and control paths should deliberately ignore the result:

```cpp
(void)dma_log.write(
    mnc::logging::Priority::warning,
    "DMA read temporarily unavailable",
    "dma_read_unavailable");
```

Do not retry in a tight loop, block acquisition, terminate a service, or write a
second fallback log merely because logging failed.

### Convenience wrappers and source locations

If an application introduces a convenience wrapper, it must forward the
original `std::source_location`. Otherwise every entry will incorrectly point
to the helper itself:

```cpp
#include <initializer_list>
#include <source_location>
#include <span>

void log_message(
    const mnc::logging::Logger& logger,
    mnc::logging::Priority priority,
    std::string message,
    std::string_view event,
    std::initializer_list<mnc::logging::Field> fields = {},
    const std::source_location& source =
        std::source_location::current())
{
    (void)logger.write(
        priority,
        message,
        event,
        std::span<const mnc::logging::Field>{
            fields.begin(), fields.size()},
        source);
}
```

`Logger::write()` remains the underlying unified API. A wrapper should add only
call-site convenience, not different logging policy or storage behavior.

## 3. Usage Demo

The following example logs a complete device lifecycle:

```cpp
#include <mnc/logging/logging.hpp>

#include <array>
#include <cerrno>
#include <cstring>
#include <string>

namespace {

const mnc::logging::Logger dma_log{"fpga-acquisition", "dma"};

} // namespace

bool open_meter_device(const std::string& path)
{
    dma_log.write(
        mnc::logging::Priority::info,
        "opening meter DMA device: " + path,
        "dma_open_requested",
        std::array<mnc::logging::Field, 1>{{
            {"MNC_DEVICE", path},
        }});

    const int fd = open_device(path); // Application-specific operation.
    if (fd < 0) {
        dma_log.write(
            mnc::logging::Priority::error,
            "failed to open meter DMA device: " +
                std::string{std::strerror(errno)},
            "dma_open_failed",
            std::array<mnc::logging::Field, 2>{{
                {"MNC_DEVICE", path},
                {"MNC_ERRNO", std::to_string(errno)},
            }});
        return false;
    }

    dma_log.write(
        mnc::logging::Priority::notice,
        "meter DMA device is ready",
        "dma_ready",
        std::array<mnc::logging::Field, 1>{{
            {"MNC_DEVICE", path},
        }});
    return true;
}
```

Inspect all MSAP1 logs in timestamp order:

```sh
mnc log
```

Filter the demo entries:

```sh
mnc log --component fpga-acquisition --module dma
mnc log --component fpga-acquisition --priority warning
mnc log --since "10 minutes ago"
mnc log --follow
mnc log --json
```

The equivalent low-level journal inspection is:

```sh
journalctl MNC_COMPONENT=fpga-acquisition MNC_MODULE=dma
```

## Reading Logs Programmatically

Use `JournalReader` for bounded queries and cursor continuation:

```cpp
#include <mnc/logging/journal_reader.hpp>

#include <iostream>

mnc::logging::JournalReader reader;
if (!reader.available())
    return;

mnc::logging::Query query;
query.component = "fpga-acquisition";
query.module = "dma";
query.limit = 100;

const auto entries = reader.read(query);
for (const auto& entry : entries)
    std::cout << mnc::logging::entry_to_json(entry) << '\n';

if (!entries.empty()) {
    mnc::logging::Query next_page = query;
    next_page.after = entries.back().cursor;
    const auto following_entries = reader.read(next_page);
}
```

Use the returned journal cursor for pagination. Do not paginate by timestamp
alone because multiple services can produce entries with the same timestamp.

For a live stream, use `JournalReader::follow()` and return `false` from the
entry handler, or from the optional continuation callback, when the caller
wants to stop.
