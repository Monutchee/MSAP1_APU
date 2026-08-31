# MSAP1 Linux meter acquisition

`msap1-settings` is the sole persistent settings authority, while
`msap1-fpga-acquisition` is the sole Linux owner of meter-result DMA and the
R5 core 0 RPMsg control endpoint. Acquisition consumes an immutable typed
settings snapshot, converts it to fixed-point PL coefficients, commits it
through the RPU, starts capture, and caches fixed 256-byte meter records from
`/dev/msap1-meter`, including 10/12-cycle Basic and 150/180-cycle aggregate
records.

The runtime data paths are:

```text
AD7771 capture -> PL conversion -> PL RMS + VLA frequency -> AXI DMA
    -> /dev/msap1-meter -> msap1-fpga-acquisition
    -> typed decoder -> R5C1 ENERGY family assembler
    -> msap1-meter-stream SQLite lifetime ledger
    -> latest-period MeterDataProvider after durable acknowledgement
    -> CLI, authenticated JSON API, and snapshot publishers

msap1-meter-historian -> typed, paged historian IPC
    -> reusable mnc::datalogger aggregation + JSON/CSV writer
    -> msap1-data-sender durable outbox
    -> Local-only archive or independent HTTP(S)/FTP/SFTP deliveries

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

The project uses C++23, Boost.Asio, Glaze 8.0.0, OpenAMP-helper-APU, WebEngine,
SQLite, and libcurl. The product-neutral `mnc::datalogger` target contains the
generation, serialization, scheduling, channel, and outbox contracts; the
`msap1::datalogger` target supplies the MSAP1 historian and IPC adapters.
The reusable `mnc::logging` library writes and reads structured systemd
journal entries; MSAP1-specific component/event policy remains in this
repository.

## Modbus protocol gateway

`msap1-modbus-server` publishes the latest coherent Basic meter snapshot over
Modbus TCP and any enabled Modbus RTU ports. It does not own a measurement
cache, DMA device, or database: the product register adapter uses the typed
`MeterSnapshotProvider`, and one Modbus block read maps one acquisition
snapshot into all requested registers.

The product-neutral `mnc::modbus` library provides asynchronous Boost.Asio
TCP/RTU transports and shared FC03, FC04, FC06, and FC10 request processing.
The initial MSAP1 register contract is read-only; FC06/FC10 therefore return
Illegal Data Address until a reviewed writable product register is added.
Runtime TCP and RTU communication settings are part of the central
`active.json` settings document and hot-reload without restarting acquisition.
`modbus-map-dump --format json` exports the compiled register contract as a
versioned machine-readable document for Python, spreadsheet, and customer
documentation generators.
See [Modbus server architecture](docs/System_Architecture/MODBUS_SERVER.md)
for the register table, byte order, quality rules, and extension workflow.

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
are all part of this one schema-version-5 product document. Schema versions
1 through 4 migrate in place to version 5 with empty Data Logging channels and
jobs, so an upgrade never starts outbound traffic. No legacy `/etc` profile is
read or migrated.

Each settings update validates the complete candidate, invokes acquisition's
coordinated stop/configure/readback/restart transaction, and atomically saves
`active.json` only after successful verification. A failed apply or write
restores the prior runtime configuration. Factory reset loads the packaged
APU-owned document, clears secrets, and preserves meter records, waveforms,
logs, and firmware. The service intentionally keeps no drafts or revision
history.

`mnc meter-view` displays the user-facing meter channels CH0 through CH6.
CH7/VCM remains available in the basic record and internal API model for future
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

`metering.measurement_topology` declares whether the three voltage inputs are
`wye` (star) or `delta` and defaults to `wye`. It is presentation metadata:
the setting changes operator labels and zero-sequence guidance but does not
alter the PL/RPU sequence algorithm. `metering.system_nominal_voltage_v` is the
corresponding presentation reference—line-to-neutral for wye and line-to-line
for delta—and defaults to 120 V. Neither field rescales meter results or enters
the PL/RPU configuration ABI, so changing only these fields does not restart
capture.

Internal readers use a persistent Boost.Asio Unix-domain stream endpoint:

```text
/run/monutchee/fpga-acquisition.sock
```

The stream uses the version-1 24-byte `MNCI` envelope and explicitly
little-endian product payloads. Acquisition IPC version 20 adds typed meter
snapshot selection by period and attribute set. `MeterDataProvider` publishes
typed latest values for the Basic (10/12-cycle), 150/180-cycle, clock-aligned
10-minute, and 2-hour measurement periods, plus non-normative live partials
for the two long periods. Unavailable values are never represented as valid
zero and values never inherit between periods. See
[IPC, meter data, and service architecture](docs/IPC_SERVICE_ARCHITECTURE.md).
Latest subscriptions are intentionally lossy and are suitable for Web, CLI,
Modbus, and telemetry publishers. Durable historian delivery is implemented
by `msap1-meter-stream` and `msap1-meter-historian`; see
[Meter data streaming and historian architecture](common/mnc/MeterDataProvider/stream/README.md).

## Meter Data Sender and reusable Datalogger

M19 adds one canonical meter attribute catalog under
`common/mnc/MeterDataProvider/attributes/`. Snapshot and historian lists are
capability-filtered views of that catalog, including stable IDs/keys, friendly
labels, groups, units, search aliases, value kinds, supported calculations,
and period support. MQTT, history, the external API, and the Web attribute
picker consume those views rather than maintaining parallel attribute tables.

Reusable generation code lives under `common/mnc/datalogger/`; it depends on
abstract historical-data, content-writer, outbox, transfer, and clock
interfaces. The MSAP1 implementation under `common/msap1/datalogger/` queries
the typed historian IPC API. It never opens historian SQLite, acquisition IPC,
DMA, RPMsg, or device nodes. See the
[reusable Datalogger contract](common/mnc/datalogger/README.md).

`msap1-data-sender` is a dedicated `mnc::Service` daemon. It aligns completed
job windows to UTC, creates deterministic `mnc.meter.datalog.v1` JSON or CSV
artifacts, and commits them to `/data/mnc/data-sender/`. Local-only artifacts
remain in the archive without a network attempt. Remote artifacts have one
durable delivery row per selected channel and use at-least-once delivery;
already acknowledged channels are not resent when another channel fails.
Payloads remain until all selected channels acknowledge them. A quota or
free-space guard pauses new generation without deleting unsent data.

HTTP and HTTPS use a raw POST with MIME type, stable artifact identity,
filename, checksum, and idempotency headers. FTP and SFTP upload to a temporary
name and then rename to the deterministic final filename. HTTP/FTP require an
explicit insecure-transport acknowledgement, HTTPS always verifies peer and
hostname, and SFTP requires installed known-host material. Channel credentials
and TLS/SSH assets are channel-scoped settings material resolved only to the
dedicated Data Sender runtime identity; secrets never enter `active.json` or
REST responses. The Data Sender IPC contract is framed version 1 at
`/run/monutchee/data-sender/data-sender.sock`.

The authenticated external API is:

- `POST /api/login` and `POST /api/logout`
- `GET /api/v1/session`
- `GET /api/v1/health`
- `GET /api/v1/about` (viewer-safe MNCOS version, image build identifier, and
  software build date)
- `GET /api/v1/meter/health`
- `GET /api/v1/meter/attributes?usage=snapshot|historian` (canonical,
  period-aware attribute descriptors and calculation capabilities)
- `GET /api/v1/meter/readings`
- `GET /api/v1/meter/aggregate` (newest 150/180-cycle aggregate; always 200
  while acquisition answers, with `{"available": false}` until the first
  aggregate exists)
- `GET /api/v1/meter/minutes-10` (newest finalized clock-aligned 10-minute
  aggregate; always 200 while acquisition answers, with
  `{"available": false}` until the first clean interval exists)
- `GET /api/v1/meter/hours-2` (newest finalized two-hour aggregate; always 200
  while acquisition answers, with `{"available": false}` until twelve clean
  ten-minute intervals have completed)
- `GET /api/v1/meter/minutes-10/live` and
  `GET /api/v1/meter/hours-2/live` (non-normative open-interval previews)
- `GET /api/v1/meter/energy` (durable lifetime active import/export, apparent,
  and fundamental reactive quadrant I--IV counters)
- `GET /api/v1/meter/demand` (newest durable configured fixed/sliding signed
  active demand, profile metadata, and authoritative import/export peaks)
- `GET /api/v1/meter/power-quality` (M12 live Urms(1/2) diagnostics)
- `GET /api/v1/meter/power-quality/events` (durable M18 event catalogue;
  optional `event_id`, `start_utc_ns`, `end_utc_ns`, and `limit` filters)
- `DELETE /api/v1/meter/power-quality/events` (administrator only; explicitly
  confirmed selected UUIDs or complete-catalogue deletion; MNCWF files are
  retained)
- `GET /api/v1/meter/flicker` (latest independent live Pinst, Pst, and Plt)
- `GET /api/v1/meter/mains-signalling` (latest configured-carrier observation)
- `POST /api/v1/meter/energy/reset` (administrator only; resets all 28 energy
  counters with expected-epoch/idempotency protection)
- `POST /api/v1/meter/demand/peaks/reset` (administrator only; resets all
  active-demand peaks with expected-epoch/idempotency protection)
- `GET /api/v1/meter/history/capabilities`
- `POST /api/v1/meter/history/query`
- `GET /api/v1/meter/history/health`
- `GET /api/v1/data-logging/configuration` (administrator only)
- `PUT /api/v1/data-logging/configuration` (administrator only)
- `GET /api/v1/data-logging/status`
- `GET /api/v1/data-logging/artifacts` and
  `GET /api/v1/data-logging/artifact`
- `GET /api/v1/data-logging/artifacts/preview` and
  `GET /api/v1/data-logging/artifacts/download` (authenticated, manifest-
  authorized generated content; no raw filesystem alias)
- `POST /api/v1/data-logging/artifacts/retry` and
  `DELETE /api/v1/data-logging/artifacts` (administrator only; discarding
  incomplete delivery requires explicit `discard_unsent` confirmation)
- `POST /api/v1/data-logging/channels/test` (administrator-only zero-data
  saved-channel probe)
- `GET /api/v1/data-logging/channel-materials`,
  `PUT/DELETE /api/v1/data-logging/channel-credential`, and upload/delete
  `/api/v1/data-logging/channel-asset` (administrator only; presence/status is
  returned, never secret material)
- `GET /api/v1/developer/database` (administrator only)
- `PUT /api/v1/developer/database` (administrator only)
- `POST /api/v1/developer/database/maintenance` (administrator only; clears
  selected historian projections while preserving the raw-record spool)
- `GET /api/v1/meter/configuration/frequency`
- `PUT /api/v1/meter/configuration/frequency`
- `GET /api/v1/waveforms`
- `POST /api/v1/waveforms/trigger` (administrator only)
- `DELETE /api/v1/waveforms` (administrator only; deletes one session or, with
  explicit confirmation, all inactive sessions and their MNCWF files)
- `GET /api/v1/waveforms/export?session_id=<id>&event_id=<uuid>&format=mncwf`
  (authenticated viewer; streamed virtual event capture)
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

Waveform session summaries classify their contributing triggers as `manual`,
`power_quality`, `mixed`, or `legacy`. This is presentation provenance only:
manual and PQ capture still share the same capture coordinator, MNCWF-v4
storage, parser, and export path.

`GET /api/v1/meter/readings` reports the ~200 ms cycle-defined basic block;
`GET /api/v1/meter/aggregate` reports the ~3 s 150/180-cycle aggregate R5C1
folded from 15 basic blocks; `GET /api/v1/meter/minutes-10` reports the
independently aligned 10-minute aggregate; and `GET /api/v1/meter/hours-2`
reports twelve consecutive clean ten-minute intervals. Each finalized
aggregate endpoint exposes the typed RMS, line-line, power, phasor, and
unbalance values produced for that period. The periods never inherit from one
another. The 150/180-cycle aggregate's `frequency` object is
**informative only** — the standardized Class A frequency product is defined
over its own 10 s interval, which is not implemented — so it carries
`"informative": true` and deliberately no validity flag, and consumers must
never present it as a Class A frequency measurement. The aggregate's
`time_quality` (`"unsynchronized"`, `"synchronized"`, or `"holdover"`) is the
UTC synchronization state captured when that aggregate was measured, not the
daemon's state when the request arrived: an aggregate measured while
synchronized keeps reporting `"synchronized"` even if the clock has since
dropped into holdover. Frequency is intentionally unavailable in the
10-minute and 2-hour results because the standardized frequency product uses
its own 10-second interval.

Every available meter attribute exposes `key`, `unit`, `value`, the legacy
`valid` boolean, exact `quality`, and `source_sequence`. `quality` is one of
`valid`, `unavailable`, `invalid`, `out_of_range`, `timed_out`, or
`arithmetic_error`; consumers must not collapse those states into a numeric
zero. `valid` remains equivalent to `quality == "valid"` for API-v1 clients.
Available basic, 150/180-cycle, 10-minute, and two-hour records also expose
`record_complete`. It is true only when every supported derived attribute was
published from the record's top-level sequence. The pending
`{"available": false}` shapes are unchanged. Basic timing and available
aggregate records may expose `utc_start_nanoseconds` and
`utc_uncertainty_nanoseconds`; absence means measurement UTC is unavailable
and must not be replaced with HTTP request time.

The power catalog uses the meter's authoritative totals. Positive active power
means import and negative active power means export. The current
`power.reactive.*` result is the signed fundamental reactive power Q₁:
positive is inductive/lagging and negative is capacitive/leading. Backend
apparent power S is an independent authoritative quantity and need not equal
the conventional P–Q₁ resultant `sqrt(P² + Q₁²)` in distorted or unbalanced
conditions. Clients must not sum phase values to construct totals or label the
difference as distortion power. The API does not currently publish THD,
fundamental P₁/S₁ or power algorithm profile/version;
those fields must remain unavailable rather than inferred.

Energy quadrant selection uses simultaneous active power P and fundamental
reactive power Q1 signs: quadrant I is `P>=0,Q1>0`, II is `P<0,Q1>0`, III is
`P<0,Q1<0`, and IV is `P>=0,Q1<0`. `P==0` stays on the import side and
`Q1==0` adds no reactive energy. Phase counters use phase signs; the total
counter uses algebraic total signs and is never reconstructed from phase
quadrants. All browser-facing 64-bit counters, session IDs, sample anchors,
and reset epochs are decimal strings so values beyond JavaScript's safe
integer range remain exact.

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
mnc meter energy
mnc --output json meter energy
mnc meter energy reset --expected-epoch 0 \
    --idempotency-key commissioning-energy-1 --yes
mnc meter demand
mnc --output json meter demand
mnc meter demand peaks-reset --expected-epoch 0 \
    --idempotency-key commissioning-demand-1 --yes
mnc meter power-quality
mnc meter power-quality events --limit 100
mnc meter power-quality events \
    --event 01234567-89ab-5def-8123-456789abcdef
mnc meter flicker
mnc meter mains-signalling
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
mnc waveform export --session 17 \
    --event 01234567-89ab-5def-8123-456789abcdef --format mncwf \
    --file event-17.mncwf
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

New `.mncwf` version 4 files store CH0 through CH6 as signed 32-bit raw counts
or explicitly identified boxcar-decimated averages and deliberately omit
diagnostic CH7. Versioned sections freeze exact channel transforms, timebase,
quality, events, capture/device identity, and UUID lineage so later COMTRADE or
PQDIF export needs only the master file. Legacy versions 1 through 3 remain
discoverable. See the
[MNCWF v4 contract](docs/File_Standard/MNCWF_V4_FILE_FORMAT.md) for the exact
binary layout. R5 firmware and RPMsg are not in this payload path.

Event export validates and read-only maps the completed v4 master, rebuilds
only the selected virtual slice's metadata/directory/CRCs, and streams the
unchanged sample extent in bounded chunks. It does not persist a second master
on the device. Both the API and CLI advertise only `mncwf`; `comtrade` and
`pqdif` fail explicitly until the later protocol-gateway converters land.
The acquisition daemon delivers each event/capture UUID association to the
durable historian on an isolated retry worker, so historian startup or ingest
lag can never block DMA. Catalogue results and the Web UI join those capture
UUIDs to the daemon-reported session/master/continuation identities.

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

Record-rejection health is scoped to the current deliberate capture epoch, so
a recovered configuration test does not leave the meter permanently failed.
The process-lifetime rejection total remains beside it for forensics. In ADC
simulator mode, the full report labels SPI, physical DCLK, and the AD7771
register snapshot as not applicable rather than presenting zero-valued
physical diagnostics as failures.

Both summaries carry a `DMA transport` line with the kernel driver's own ring
accounting: blocks produced, blocks consumed, overruns, cyclic completion
callbacks, and the ring depth. The Xilinx cyclic callback fires per interrupt
rather than per period, so the reported deficit (produced minus callbacks) is
the number of period completions the driver coalesced. These counters are
observability only — no health verdict depends on them — but a sequence gap
with matching overrun growth is a consumer stall, while one without it happened
upstream in PL. The same values appear under `acquisition.dma_transport` in the
JSON output and in the Web health document.

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
