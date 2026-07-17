# MSAP1 APU repository guidance

## Purpose and routing

- This repository builds `msap1-fpga-acquisition`, the Linux-side AD7771 IIO
  acquisition daemon, and `msap1-apu-app`, its diagnostic client.
- Read `README.md` before changing behavior. For ADC bring-up expectations and
  known limitations, read `tests/method/test_adc_rpmsg_procedure.md` instead of
  recording transient results here.
- Public headers are under `include/msap1/`; implementation is under `src/`.
- `libs/openamp-helper` is the shared Linux RPMsg transport submodule. Keep
  service discovery, `rpmsg_chrdev` binding, and generic endpoint I/O there;
  keep MSAP1 wire-protocol handling in this repository.

## Architecture contract

- R5 core 0 owns AD7771 SPI control, reset/synchronization, and PL capture
  registers. Linux exclusively owns AXI DMA S2MM, scatter-gather descriptors,
  interrupts, and CMA-backed DDR buffers through IIO/DMAengine.
- `msap1-fpga-acquisition` is the sole IIO and RPMsg lifecycle owner. It arms
  IIO before requesting capture START, requests STOP before disabling IIO, and
  publishes the full-rate stream through the shared-memory ring.
- CLI and web consumers must use the daemon socket and shared-memory ring. Do
  not open the IIO device, RPMsg endpoint, `/dev/spidev*`, `/dev/mem`, or UIO
  from another process.
- The ADC capture rate defaults to 32,000 frames/s. `adc-view --rate` changes
  only the visualization rate; it must not silently reconfigure the ADC.
- ADC samples never travel over RPMsg. RPMsg carries only control and health.
- Every shared-ring reader owns an independent sequence cursor. A slow reader
  reports its own overrun and must never block the acquisition producer or
  another reader.
- Samples are raw signed 24-bit ADC counts represented as `int32_t`. Do not
  label them volts or amperes without board calibration and analogue transfer
  functions.

## Cross-repository ABI

- `include/msap1/rpu_control_protocol.h` is the APU copy of the wire ABI defined
  with `MSAP1_RPU/common/include/rpu_control_protocol.h`.
- Keep message numbers, status values, packed structure layout, field widths,
  and maximum frame size compatible on both sides. Update both repositories in
  the same feature and extend `tests/protocol_test.cpp` for protocol changes.
- Preserve compatibility deliberately or increment the protocol version and
  implement explicit handling on both peers.

## Build and verification

Run from the repository root:

```sh
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

- The codebase uses C++17 and should remain warning-clean under `-Wall`,
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
