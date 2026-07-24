# MSAP1 APU repository guidance

## Purpose and routing

- This repository builds `msap1-fpga-acquisition`, the Linux-side meter-record
  acquisition daemon, the `mnc` diagnostic CLI, and
  `msap1-web-backend`, the authenticated external JSON API and nginx owner.
- Read `README.md` before changing behavior. For ADC bring-up expectations and
  known limitations, read `tests/method/test_adc_rpmsg_procedure.md` instead of
  recording transient results here.
- Public headers are under `include/msap1/`; shared implementation is under
  `src/`, and each executable has its own directory under `apps/`.
- `libs/openamp-helper` is the shared Linux RPMsg transport submodule. Keep
  service discovery, `rpmsg_chrdev` binding, and generic endpoint I/O there;
  keep MSAP1 wire-protocol handling in this repository.
- `libs/webengine` and `libs/glaze` are pinned submodules. Keep WebEngine
  platform-neutral; MSAP1 routes, runtime paths, authentication policy, and
  systemd integration belong under `apps/web-backend` or `meta-msap1`.

## Architecture contract

- R5 core 0 owns AD7771 SPI control, reset/synchronization, PL capture
  registers, and the AdcConversion/MeterProcessing AXI-Lite configuration.
  Linux exclusively owns AXI DMA S2MM, scatter-gather descriptors, interrupts,
  and CMA-backed DDR buffers through `/dev/msap1-meter`.
- `msap1-fpga-acquisition` is the sole meter DMA and RPMsg lifecycle owner. It
  opens/arms DMA, commits the JSON-derived configuration through R5 core 0,
  requests capture START, and requests STOP before closing DMA.
- CLI and web consumers use the daemon's binary `SOCK_SEQPACKET` protocol at
  `/run/monutchee/fpga-acquisition.sock`. Do not open the DMA device, RPMsg
  endpoint, `/dev/spidev*`, `/dev/mem`, or UIO from another process.
- The ADC capture rate defaults to 32,000 frames/s. Complete schema-version-2
  profiles under `/etc/monutchee/msap1/default/adc_config/` define the ADC PGA,
  physical current/voltage frontends, and the 200 ms RMS window. The packaged
  runtime default is `msap1-sensor-board-5a.json`. Profiles also define CH6/VLA
  zero-crossing frequency measurement; a valid Web-generated complete profile
  is persisted as `/etc/monutchee/msap1/adc_config/active.json`.
- ADC and meter payloads never travel over RPMsg. RPMsg carries only
  configuration, control, health, and acknowledgements.
- ADC health exposes the PL-measured DCLK rate and physical `ADC_DRDY_N`
  falling-edge rate through the CLI and external JSON API. A missing or
  mismatched DRDY measurement makes ADC health fail even when SPI register
  readback matches.
- Temporary ADC rate diagnostics must flow through the acquisition daemon so
  DMA/capture, AD7771 SRC/PGA, and PL window configuration remain coherent.
  Daemon restart restores the 32 kSPS profile default.
- Linux consumes fixed 256-byte `MTR1` records. The daemon caches the newest
  coherent result, and concurrent CLI/web readers never backpressure PL.
- Voltage and current readings are RMS values calculated in PL and encoded in
  Q16 microvolts and microamps. The selected complete profile chooses
  mean-corrected AC RMS or zero-referenced total RMS and supplies the
  per-channel physical scaling and AD7771 PGA factors.

## Cross-repository ABI

- `include/msap1/rpu_control_protocol.h` is the APU copy of the wire ABI defined
  with `MSAP1_RPU/common/include/rpu_control_protocol.h`.
- Keep message numbers, status values, packed structure layout, field widths,
  and maximum frame size compatible on both sides. Update both repositories in
  the same feature and extend `tests/protocol_test.cpp` for protocol changes.
- The prototype wire version remains 2. Keep the coordinated APU/RPU copies
  byte-identical when adding configuration fields or acknowledgements.

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
