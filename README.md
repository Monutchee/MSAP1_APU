# MSAP1 Linux meter acquisition

`msap1-settings` is the sole persistent settings authority, while
`msap1-fpga-acquisition` is the sole Linux owner of meter-result DMA and the
R5 core 0 RPMsg control endpoint. Acquisition consumes an immutable typed
settings snapshot, converts it to fixed-point PL coefficients, commits it
through the RPU, starts capture, and caches fixed 256-byte `MTR1` records from
`/dev/msap1-meter`.

The runtime data paths are:

```text
AD7771 capture -> PL conversion -> PL RMS + VLA frequency -> AXI DMA
    -> /dev/msap1-meter -> msap1-fpga-acquisition
    -> typed decoder -> latest-period MeterDataProvider
    -> CLI, authenticated JSON API, and future publishers

AD7771 raw frames -> nonblocking PL waveform packetizer -> waveform AXI DMA
    -> /dev/msap1-waveform -> 128 MiB daemon history
    -> triggered .mncwf files
```

R5 core 0 retains exclusive ownership of AD7771 SPI, reset/synchronization,
capture control, and metering AXI-Lite registers. Linux owns both AXI DMA S2MM
engines, scatter-gather descriptors, interrupts, and CMA-backed DDR buffers.
RPMsg carries configuration, control, health, and acknowledgements—never
sample, waveform, or meter payloads.

## Build

```sh
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The project uses C++23, Boost.Asio, Glaze 8.0.0, OpenAMP-helper-APU, and
WebEngine.
The reusable `mnc::logging` library writes and reads structured systemd
journal entries; MSAP1-specific component/event policy remains in this
repository.

## Runtime settings

The canonical factory document is maintained in this repository at
`config/settings/factory-defaults.json` and installed byte-for-byte at:

```text
/usr/share/monutchee/msap1/settings/factory-defaults.json
```

On first boot, or when `active.json` is missing or empty, `msap1-settings`
validates the factory document and initializes
`/data/mnc/settings/active.json`. It is the only process that may mutate the
active settings or the separately protected secrets document. Frequency, RMS,
conversion, normal sample rate, ADC source/simulator, and waveform defaults
are all part of this one schema-version-1 product document. No legacy `/etc`
profile is read or migrated.

Each settings update validates the complete candidate, invokes acquisition's
coordinated stop/configure/readback/restart transaction, and atomically saves
`active.json` only after successful verification. A failed apply or write
restores the prior runtime configuration. Factory reset loads the packaged
APU-owned document, clears secrets, and preserves meter records, waveforms,
logs, and firmware. The service intentionally keeps no drafts or revision
history.

`mnc meter-view` displays the user-facing meter channels CH0 through CH6.
CH7/VCM remains available in the MTR1 record and internal API model for future
reference-monitoring support, but is intentionally hidden from the CLI.

The basic measurement block is cycle-defined — 10 grid cycles at a 50 Hz
nominal, 12 at 60 Hz (see [docs/TIMING_MODEL.md](docs/TIMING_MODEL.md)) — so
its actual length varies with grid frequency. The PL free-run fallback
window derives from `metering.nominal_frequency_hz`; at the 128 kSPS factory
default it is exactly 25,600 frames for both nominals (6,400 at 32 kSPS). The daemon
opens DMA first, stops capture, programs and verifies each AD7771 channel PGA
through the RPU, sends the derived PL coefficients, verifies the PL generation
readback, then requests capture START. Shutdown requests STOP before closing
DMA.

`metering.rms.remove_dc` selects the PL RMS reference. It defaults to `true`,
producing AC RMS about the window mean. Saving the settings hot-applies the
change; no process or device reboot is required.

Internal readers use a persistent Boost.Asio Unix-domain stream endpoint:

```text
/run/monutchee/fpga-acquisition.sock
```

The stream uses the version-1 24-byte `MNCI` envelope and explicitly
little-endian product payloads. Acquisition IPC version 20 adds typed meter
snapshot selection by period and attribute set. `MeterDataProvider` currently
publishes typed latest values for the Basic (10/12-cycle) and 150/180-cycle
measurement periods. The generic period vocabulary reserves 10 min and 2 h
for future PL products, but provider capabilities do not advertise them until
measurements exist. Unavailable values are never represented as valid zero and
values never inherit between periods. See
[IPC, meter data, and service architecture](docs/IPC_SERVICE_ARCHITECTURE.md).
Latest subscriptions are intentionally lossy and are suitable for Web, CLI,
Modbus, and telemetry publishers. Durable historian delivery is a separate
future pipeline described in
[FUTURE_DURABLE_METER_PIPELINE.md](common/mnc/MeterDataProvider/FUTURE_DURABLE_METER_PIPELINE.md).

The authenticated external API is:

- `POST /api/login` and `POST /api/logout`
- `GET /api/v1/session`
- `GET /api/v1/health`
- `GET /api/v1/about` (viewer-safe MNCOS version, image build identifier, and
  software build date)
- `GET /api/v1/meter/health`
- `GET /api/v1/meter/readings`
- `GET /api/v1/meter/aggregate` (newest 150/180-cycle aggregate; always 200
  while acquisition answers, with `{"available": false}` until the first
  aggregate exists)
- `GET /api/v1/meter/configuration/frequency`
- `PUT /api/v1/meter/configuration/frequency`
- `GET /api/v1/waveforms`
- `POST /api/v1/waveforms/trigger` (administrator only)
- `GET /protected/waveforms/view/<filename>` (authenticated viewer)
- `GET /protected/waveforms/download/<filename>` (authenticated viewer,
  attachment)
- `GET /api/v1/developer/logs` (administrator only; bounded journal page with
  `component`, `module`, `priority`, `after`, and `limit` query parameters)
- `GET /api/v1/developer/temperatures` (administrator only; label-discovered
  LPD, FPD, and PL temperatures)
- `GET /api/v1/developer/about` (administrator only; MD5 diagnostic
  fingerprints for the deployed PL bitstream, R5 firmware, and APU
  executables)
- `GET /api/v1/adc/capture`
- `PUT /api/v1/adc/capture` and `DELETE /api/v1/adc/capture`
- `GET /api/v1/adc/source` and `PUT /api/v1/adc/source`
- `GET /api/v1/adc/simulator` and `PUT /api/v1/adc/simulator`
- `GET /api/v1/settings/active`
- `PUT /api/v1/settings/active` (administrator only)
- `POST /api/v1/settings/factory-reset` (administrator only)

`GET /api/v1/meter/readings` reports the ~200 ms cycle-defined basic block;
`GET /api/v1/meter/aggregate` reports the ~3 s 150/180-cycle aggregate the PL
folded from 15 basic blocks. The two never mix: the readings document always
comes from a basic record. The aggregate's `frequency` object is
**informative only** — the standardized Class A frequency product is defined
over its own 10 s interval, which is not implemented — so it carries
`"informative": true` and deliberately no validity flag, and consumers must
never present it as a Class A frequency measurement. The aggregate's
`time_quality` (`"unsynchronized"`, `"synchronized"`, or `"holdover"`) is the
UTC synchronization state captured when that aggregate was measured, not the
daemon's state when the request arrived: an aggregate measured while
synchronized keeps reporting `"synchronized"` even if the clock has since
dropped into holdover.

External responses use JSON. The development login is `admin` / `admin`.
The backend owns nginx and serves HTTP 80 and HTTPS 443. Read-only routes
require the viewer role; changing ADC capture requires the admin role.
The Developer About fingerprints identify whether deployed files match; MD5 is
not used as a security or integrity guarantee.

## Commands

```sh
mnc meter health
mnc meter health --full
mnc meter health --refresh
mnc meter snapshot
mnc meter view
mnc meter view --results 20
mnc meter view --duration 10
mnc adc stop
mnc adc start
mnc adc rate
mnc adc rate --sps 16000
mnc adc testflw --flow 1
mnc adc source
mnc adc source --set simulator
mnc adc simulator show
mnc adc simulator configure --frequency-hz 60 \
    --va-rms 120 --vb-rms 120 --vc-rms 120 \
    --ia-rms 5 --ib-rms 5 --ic-rms 5 --in-rms 0
mnc waveform status
mnc waveform trigger --pre-ms 10000 --post-ms 10000
mnc waveform list
mnc log
mnc log --component fpga-acquisition
mnc log --module dma --priority warning
mnc log --since "10 minutes ago" --follow
mnc log --json
mnc system temperature
mnc machine describe
mnc --output json meter health
mnc service list
mnc service status fpga-acquisition
mnc service restart web-backend
```

`mnc system temperature` discovers the ZynqMP LPD, FPD, and PL sensors from
their `Temp_LPD`, `Temp_FPD`, and `Temp_PL` hwmon labels. It deliberately does
not depend on a fixed `/sys/class/hwmon/hwmonN` index because Linux may assign
that index differently across boots and kernel versions.

`mnc waveform trigger` records a manual event against the newest raw ADC
sequence. The daemon retains 128 MiB of raw eight-channel frames (about
32 seconds at the 128 kSPS default, 131 seconds at 32 kSPS), so the
resulting file can include samples that
precede the trigger. Overlapping manual or future PQ-event windows are merged
into one longest capture and retain every event marker. Each trigger refreshes
an uncertainty-bounded `CLOCK_TAI`/PL-tick correlation through the separate
waveform AXI-Lite registers. The acquisition loop snapshots a completed
session from history and a background writer publishes it atomically, so
filesystem latency cannot block DMA draining. A session that intersects a
reported transport gap is marked incomplete and is not published as a valid
capture. Completed files survive service and system restarts under:

```text
/data/mnc/waveform/
```

New `.mncwf` version 2 files store CH0 through CH6 as raw signed 32-bit counts
and deliberately omit diagnostic CH7. Channel descriptors carry names and the
active Q16.16 conversion coefficients, allowing raw-count and converted-unit
views without duplicating the sample payload. Legacy eight-channel version 1
files remain readable. See [the MNCWF reader guide](docs/MNCWF_FILE_FORMAT.md)
for the exact binary layout, conversion equation, and a Python example. R5
firmware and RPMsg are not in this payload path.

`mnc log` combines acquisition, web-backend/nginx, PL-load, and RPU-load
events in timestamp order. Structured entries identify their process component,
internal module, stable event name, source location, and optional request or
configuration generation. `--priority warning` includes warnings and all more
severe levels. `--json` emits one object per journal entry for automation.
Journal cursors back the public C++ reader API so a future bounded MCP log
reader can continue reliably across equal timestamps and journal rotation.
Reading the complete system journal requires root or membership in the
`systemd-journal` group; product services need no special permission to write.
For bounded automation, global `--output json` returns one response envelope
and `--cursor` continues a log query. The older `mnc log --json` JSONL output
remains a local compatibility interface.

`mnc adc rate` compares the requested rate, SRC-register-derived rate, and
physical DRDY rate. `--sps` temporarily selects one of 1, 2, 4, 8, 16, 32, 64,
or 128 kSPS through a coordinated DMA/capture stop, AD7771 SRC/PGA update, PL
window update, and restart. It polls across one-second PL measurement windows
until two consecutive DRDY readings agree, avoiding stale pre-change results.
The complete profile remains unchanged and the selection is not persisted;
restarting the acquisition daemon restores the persisted profile rate
(128 kSPS factory default).

`mnc adc testflw --flow 1` is a destructive, self-restoring rate diagnostic.
The daemon stops DMA/capture, asks R5c0 to pulse the PL-driven sensor-board
`ADC_RESET_N` output, records the reset-default registers, performs an
observable 1 ms `SRC_UPDATE` high/low load, waits for fresh PL DCLK/DRDY
measurement windows, and restores the prior running state. This is a warm ADC
pin reset: it does not power-cycle the ADC and does not reset Linux or the
FPGA. The command takes about five seconds and prints copyable before,
reset-asserted, reset-default, and after snapshots.

`mnc meter health` reports the daemon's cached RPU audit together with the
one-second meter-record freshness check in a concise summary. Use
`mnc meter health --full` for complete pipeline counters, SPI diagnostics,
decoded AD7771 controls, and the raw register snapshot. The full 100-register
SPI audit runs
after a two-second capture-start stabilization interval, after configuration
changes, and every 30 seconds. The startup delay lets the PL DRDY meter publish
a complete one-second capture-active window before its rate is evaluated. Use
`mnc meter health --refresh` when an immediate destructive-on-the-bus audit is
needed. One failed SPI audit is retained as a pending confirmation and retried
after one second; only two consecutive failures replace a known-good health
snapshot. Recovered per-register retries and the last malformed register/header
are included in the output.

The command also reports both the number of AXI packets accepted by the
capture stream, external AD7771 DCLK frequency, and `ADC_DRDY_N` frame rate
measured in PL. The DCLK and DRDY fields are `unavailable` until two one-second
observation windows complete, and whenever the corresponding signal stops. It
also prints the complete active AD7771 channel/general/DOUT/SRC configuration,
offset/gain calibration, channel/saturation/general errors, enables, and status
register snapshot read over SPI by R5c0. A decoded summary includes the power
mode, digital filter, DOUT topology, DCLK divisor, reference mux, SRC
decimation, and the ODR implied by the measured DCLK. This keeps Linux read-only
while providing enough raw state to diagnose ADC rate and alert faults.

Run `mnc`, `mnc help meter`, or append `--help` to any command for contextual
help. Global `--socket`, `--timeout-ms`, and `--output text|json` options are
accepted before or after the command path. `mnc machine describe` prints the
authoritative command/access table used by the restricted diagnostic gateway.
See [docs/MACHINE_INTERFACE.md](docs/MACHINE_INTERFACE.md) for JSON envelopes,
cursor pagination, access classes, and the temporary opt-in SSH test account.
The Yocto package installs Bash completion for command groups, actions,
options, and socket paths.

`mnc meter health` requires matching configuration generations, responsive SPI,
measured DRDY within 1% of the configured sample rate, active capture, zero
DMA/header/FIFO errors, and a meter record no older than one second. Web health
polling reads this same cache and never starts an SPI audit.
`mnc meter view` displays RMS results and the latest VLA grid frequency. An
absent/out-of-range grid is reported as unavailable without failing acquisition;
a frequency arithmetic fault does fail meter health. RMS uses the configured
`remove_dc` mode, while frequency always uses actual signal zero crossings.
CH0/ILA through CH3/ILN are amps when the selected current-sensor definition is
enabled; CH4/VLC through CH6/VLA are volts. The packaged mV profile deliberately
leaves current invalid until a complete customized profile supplies the
sensor's primary-current rating.
