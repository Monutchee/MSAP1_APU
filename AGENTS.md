# MSAP1 APU repository guidance

## Purpose and routing

- This repository builds `msap1-fpga-acquisition`, the Linux-side meter-record
  acquisition daemon, the `mnc` diagnostic CLI, and
  `msap1-web-backend`, the authenticated external JSON API and nginx owner.
- Read `README.md` before changing behavior. For ADC bring-up expectations and
  known limitations, read `tests/method/test_adc_rpmsg_procedure.md` instead of
  recording transient results here.
- Shared libraries live under `common/<namespace>/<library>/` (for example
  `common/msap1/meter/`), with each library's headers and sources together and
  larger libraries split into feature subdirectories (for example
  `common/msap1/acquisition/{ipc,rpu,dma}/`). The include root is `common/`,
  so includes name the library: `#include "msap1/meter/meter_record.hpp"`.
  Each executable has its own directory under `apps/`, grouped by delivery
  group: `apps/MeterCore/Services/<service>/` holds the metering daemons, and a
  future protocol-gateway group gets its own `apps/<Group>/` beside it rather
  than being folded into MeterCore. Tools that serve every group stay at the
  top level (`apps/mnc-cli/`). Each group directory owns the
  `add_subdirectory` list for its own members, so the repository root only
  names groups.
- Reusable Linux logging interfaces live under `common/mnc/logging` and use
  the `mnc::logging` namespace. Keep product-neutral journal writer/query
  mechanics there and MSAP1 component, event, and access policy in this
  repository's applications.
- `libs/openamp-helper` is the shared Linux RPMsg transport submodule. Keep
  service discovery, `rpmsg_chrdev` binding, and generic endpoint I/O there;
  keep MSAP1 wire-protocol handling in this repository.
- `libs/webengine` and `libs/glaze` are pinned submodules. Keep WebEngine
  platform-neutral; MSAP1 routes, runtime paths, authentication policy, and
  systemd integration belong under `apps/MeterCore/Services/web-backend` or
  `meta-msap1`.

## Architecture contract

- R5 core 0 owns AD7771 SPI control, reset/synchronization, PL capture
  registers, and the AdcConversion/MeterProcessing AXI-Lite configuration.
  Linux exclusively owns AXI DMA S2MM, scatter-gather descriptors, interrupts,
  and CMA-backed DDR buffers through `/dev/msap1-meter`.
- `msap1-fpga-acquisition` is the sole meter DMA and RPMsg lifecycle owner. It
  opens/arms DMA, commits the JSON-derived configuration through R5 core 0,
  requests capture START, and requests STOP before closing DMA.
- The daemon is also the sole `/dev/msap1-waveform` owner. It maintains the
  128 MiB multi-trigger raw history, refreshes the PL tick/CLOCK_TAI
  correlation at each trigger, merges overlapping trigger windows, and writes
  completed `.mncwf` files asynchronously below persistent storage at
  `/data/mnc/waveform`. Persisted files contain CH0 through CH6 raw counts plus
  profile conversion metadata; CH7 remains available in live transport/debug
  logic but is omitted from product capture files. A session intersecting a transport sequence
  gap is incomplete and must not be materialized as a valid capture. Other
  processes request captures through the daemon IPC/API and never open the
  waveform DMA directly.
- Completed waveform history is rediscovered from persistent storage at daemon
  startup. Authenticated Web viewers may inspect or download captures through
  nginx's `/protected/waveforms/` routes; never expose the storage directory
  without WebEngine authorization.
- The daemon also owns full RPU ADC register-health audits. Run them after the
  capture-start measurement window has stabilized, after coordinated
  configuration changes, and on the low-rate periodic schedule. CLI/Web health
  polling reads the cache; fast pipeline failure detection uses meter-record
  freshness rather than repeated SPI sweeps.
- Product services emit structured journald entries and `mnc log` provides the
  consolidated reader. Journald remains the only log store; do not add a
  parallel product log file or database.
- `mnc` command handlers collect typed results once and select either a human
  text or machine JSON generator. Command access metadata is authoritative for
  both `mnc machine describe` and restricted remote execution; never maintain
  a second remote-command allowlist.
- Restricted diagnostics may execute only metadata-classified `diagnostic`
  commands with bounded JSON output. Runtime control, maintenance audits,
  continuous output, socket overrides, and timeout overrides remain local.
- Web journal inspection is administrator-only and must remain a bounded,
  cursor-paginated backend query. Do not expose unrestricted journal access or
  allow the browser to open journald directly.
- SoC temperature monitoring discovers the LPD, FPD, and PL sensors by their
  hwmon labels. Never bind product behavior to a particular `hwmonN` index.
- CLI, web, and future publisher consumers use the versioned `MNCI` framed
  Boost.Asio Unix-stream protocol at
  `/run/monutchee/fpga-acquisition.sock`. Product messages use explicit
  little-endian serialization; never transmit native C++ structure padding.
  Do not open the DMA device, RPMsg endpoint, `/dev/spidev*`, `/dev/mem`, or
  UIO from another process.
- Normal Web, CLI, Modbus, and telemetry consumers read typed latest snapshots
  through `MeterDataProvider`. Latest subscriptions are intentionally lossy,
  must not block acquisition, and must preserve the distinction between a
  valid zero and an unavailable attribute. Every validated PL record must be
  committed to `msap1-meter-stream` before latest-state publication.
  `msap1-meter-historian` acknowledges its independent spool cursor only after
  its typed historical projection commits; do not bypass this ordering or let
  a lossy subscriber block either durable service.
- Harmonic records are family-atomic. Acquisition buffers the 42 direct
  HARMONIC-v1 chunks for one exact provenance tuple and publishes nothing
  until the family is complete; its IPC reader drains bounded batches and must
  never wait on SQLite. Meter-stream accepts batches of up to 256 records in
  one transaction. R5C1 HARMONIC_AGG-v1 families have independent latest slots
  for 150/180-cycle, 10-minute, and 2-hour periods; API/CLI default to the
  150/180-cycle view and require an explicit period for the others. Direct
  10/12-cycle spectra remain latest-only during target proof. The historian
  persists only complete 42-record aggregate families in their three typed
  datasets and acknowledges a cursor only after the selected projection is
  durable.
- Product-neutral Modbus framing, request handling, CRC, and asynchronous
  TCP/RTU transports live under `common/mnc/modbus`. The source-controlled
  MSAP1 register contract and its `MeterSnapshotProvider` adapter live under
  `common/msap1/modbus`; Modbus code must never open acquisition IPC, DMA, or
  database resources directly. Each multi-register electrical read acquires
  exactly one coherent meter snapshot. The initial map is read-only even
  though the common engine implements FC06 and FC10 for future products.
- `mnc::Service` provides lifecycle, readiness, watchdog, reload, and shutdown
  behavior for product daemons. `msap1-service-manager` orders/adopts settings,
  acquisition, and web systemd units through sd-bus; systemd remains the only
  process supervisor and restart-policy owner.
- Link new code against the focused `msap1::meter`, `msap1::waveform`,
  `msap1::acquisition`, `msap1::system`, or `msap1::service-protocol` target.
  `msap1::apu-core` is a compatibility umbrella, not the default dependency.
  Maintain the dependency direction from value types to core libraries to
  adapters to applications. Meter and waveform libraries must not depend on
  WebEngine, CLI presentation, or product IPC transport.
- The acquisition process owns device descriptors through RAII adapters.
  HTTP handlers use `msap1::web::AcquisitionGateway`; they must not construct
  MNCI frames, correlation IDs, or acquisition payload byte streams directly.
- `msap1-settings` is the sole persistent settings authority. The canonical
  factory document is `config/settings/factory-defaults.json`; Yocto installs
  that exact file under `/usr/share/monutchee/msap1/settings/`, while the
  active document and secret state belong under `/data/mnc/settings/`.
  Missing, empty, or invalid active settings recover atomically from the
  packaged factory document. Acquisition, CLI, and Web code must
  use the typed settings IPC API rather than reading or writing product
  configuration files. Do not add drafts or revision history, reintroduce
  legacy `/etc` ADC profiles, or duplicate factory values in packaging recipes.
- The ADC capture rate defaults to 128,000 frames/s. Persistent changes to the
  ADC PGA/frontend conversion, RMS, frequency, ADC source/simulator, or
  waveform defaults must be saved through the settings authority and
  hot-applied by acquisition. `mnc adc rate --sps` remains an explicitly
  temporary diagnostic override.
- Source and simulator changes must use the daemon's coordinated stop,
  configure, readback, rollback, DMA re-arm, and restart transaction. Convert
  engineering RMS settings to signed-24-bit peak counts before crossing RPMsg;
  reject overflow rather than wrapping. Simulator mode must expose simulator
  health while marking physical SPI diagnostics not applicable.
- ADC, meter, and waveform payloads never travel over RPMsg. RPMsg carries only
  configuration, control, health, and acknowledgements.
- ADC health exposes the PL-measured DCLK rate and physical `ADC_DRDY_N`
  falling-edge rate through the CLI and external JSON API. A missing or
  mismatched DRDY measurement makes ADC health fail even when SPI register
  readback matches.
- Temporary ADC rate diagnostics must flow through the acquisition daemon so
  DMA/capture, AD7771 SRC/PGA, and PL window configuration remain coherent.
  Daemon restart restores the persisted profile rate (128 kSPS factory
  default).
- Linux consumes fixed 256-byte `MTR1` records. The daemon caches the newest
  coherent result, and concurrent CLI/web readers never backpressure PL.
- Linux consumes fixed 32,832-byte `WFM1` blocks from the independent waveform
  DMA. WFM1 carries 1024 raw eight-channel frames and 64-bit sequence/tick
  metadata; it does not replace MTR1 and does not change the RPU wire ABI.
- Voltage and current readings are RMS values calculated in PL and encoded in
  Q16 microvolts and microamps. The selected complete profile chooses
  mean-corrected AC RMS or zero-referenced total RMS and supplies the
  per-channel physical scaling and AD7771 PGA factors.
- Use neutral MSAP1 sensor-board identifiers in profile IDs, paths, CLI/API
  output, tests, and documentation. Do not introduce third-party vendor or
  product branding into this repository.

## Cross-repository ABI

- `common/msap1/acquisition/rpu/rpu_control_protocol.h` is the APU copy of the
  wire ABI defined with `MSAP1_RPU/common/include/rpu_control_protocol.h`.
- Keep message numbers, status values, packed structure layout, field widths,
  and maximum frame size compatible on both sides. Update both repositories in
  the same feature and extend `tests/protocol_test.cpp` for protocol changes.
- The prototype wire version is 6 (`MSAP1_RPU_VERSION`). Keep the coordinated
  APU/RPU copies byte-identical when adding configuration fields or
  acknowledgements.
- `msap1_meter_config_payload` is 296 packed bytes. `nominal_frequency_hz`
  (50 or 60) selects the cycles-per-block rule (50→10, 60→12) and the derived
  PL free-run fallback window; it is configuration, never inferred from
  measured frequency. The five trailing `pq_*` fields are the IEC 61000-4-30
  Urms(1/2) detection band: a reference in micro-volts (ZERO DISARMS
  detection) plus four thresholds as 1e-4 fractions of it.
- `MSAP1_RPU_MSG_SIMULATOR_EVENT_SET` drives the simulator's amplitude-event
  sequencer and is deliberately NOT part of the configuration snapshot: a
  configuration commit stops and restarts capture, which would destroy the
  phase continuity a sag/swell/interruption test depends on.
- BASIC-v4 record format `0x00010004` is the authoritative R5C1
  10/12-cycle format: word 6 = actual sample count, word 13 = timing and
  provenance (bit 19 marks the first UTC-resynchronized Basic), words 14–15
  = actual last sample, and words 9–10 = 64-bit first sample. Consecutive
  Basic records may overlap only when the later record has bit 19 and its
  range advances beyond the prior record; reject unmarked overlaps,
  contained duplicates, and forward gaps. See
  `docs/System_Architecture/TIMING_MODEL.md` for the timing model and the
  PL/RPU/APU ownership split.
- Record format `0x00020003` (AGG-v3/MTR2) is the 150/180-cycle aggregate
  fundamental record: exactly 15 consecutive eligible basic blocks folded
  by R5C1 (the authoritative aggregator) into one 256-byte record that
  interleaves with basic records on the meter DMA stream under an
  independent sequence counter. The APU only decodes MTR2 — never compute
  aggregates in APU production code — and aggregate data never travels over
  RPMsg. Status bit 3 marks the continuing UTC-overlap interval, bit 4 marks
  the synchronized interval, and words 36–37 carry the actual last sample.
  Only a bit-3 record may have a physical span shorter than its summed
  contribution count. The word layout is pinned in
  `docs/System_Architecture/TIMING_MODEL.md`.
- The newest aggregate is cached beside — never inside — the basic latest
  record, travels in `InfoResponse` behind its own presence, age, and
  time-quality fields (acquisition IPC version 19), and is published by
  `GET /api/v1/meter/aggregate`. The aggregate's `time_quality` is the
  provenance stamped at ingest (`aggregate_time_quality`), never
  `InfoResponse::time_quality`, which is the daemon's live clock state at
  reply time. `GET /api/v1/meter/readings` keeps
  reporting basic records only. The aggregate frequency is informative
  only: the standardized Class A frequency product is defined over its own
  10 s interval, which is not implemented, so the decoder keeps that
  reading's quality `unavailable` and the API publishes `informative`
  rather than a validity flag.

## Build and verification

Run from the repository root:

```sh
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

- The codebase uses C++23 and should remain warning-clean under `-Wall`,
  `-Wextra`, and `-Wpedantic`.
- Update `README.md` when CLI behavior changes. Update the ADC test procedure
  when commands, expected results, or known bring-up behavior changes.
- For target validation, run the low-rate procedure before a high-rate stress
  test and record matching PL, RPU, APU, and Yocto revisions.

## Maintaining this file

- Update this `AGENTS.md` in the same change when durable ownership, ABI,
  build, verification, or repository conventions change.
- Keep temporary failures, experiment results, and branch names in test/status
  documentation rather than here.
