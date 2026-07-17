# MSAP1 APU repository guidance

## Purpose and routing

- This repository builds `msap1-apu-app`, the Linux-side AD7771 diagnostic
  viewer and RPMsg client.
- Read `README.md` before changing behavior. For ADC bring-up expectations and
  known limitations, read `tests/method/test_adc_rpmsg_procedure.md` instead of
  recording transient results here.
- Public headers are under `include/msap1/`; implementation is under `src/`.
- `libs/openamp-helper` is the shared Linux RPMsg transport submodule. Keep
  service discovery, `rpmsg_chrdev` binding, and generic endpoint I/O there;
  keep MSAP1 wire-protocol handling in this repository.

## Architecture contract

- R5 core 0 owns AD7771 SPI control, PL capture registers, and the AXI DMA S2MM
  channel. The APU obtains control/status and a decimated visualization stream
  through RPMsg.
- Do not add direct `/dev/spidev*`, `/dev/mem`, UIO, or Linux DMA ownership
  without an explicit cross-repository architecture change.
- The ADC capture rate defaults to 32,000 frames/s. `adc-view --rate` changes
  only the visualization rate; it must not silently reconfigure the ADC.
- RPMsg sample streaming is a bring-up/visualization path, not the final
  high-throughput meter data path.
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
