# MSAP1 Linux meter acquisition

`msap1-fpga-acquisition` is the sole Linux owner of meter-result DMA and the
R5 core 0 RPMsg control endpoint. It converts the product JSON configuration
to fixed-point PL coefficients, commits them through the RPU, starts capture,
and caches fixed 256-byte `MTR1` records from `/dev/msap1-meter`.

The data path is:

```text
AD7771 capture -> PL conversion -> PL RMS processing -> AXI DMA
    -> /dev/msap1-meter -> msap1-fpga-acquisition
    -> CLI and authenticated JSON API
```

R5 core 0 retains exclusive ownership of AD7771 SPI, reset/synchronization,
capture control, and metering AXI-Lite registers. Linux owns AXI DMA S2MM,
scatter-gather descriptors, interrupts, and CMA-backed DDR buffers. RPMsg
carries configuration, control, health, and acknowledgements—never sample or
meter payloads.

## Build

```sh
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The project uses C++23, Glaze 7.9.0, OpenAMP-helper-APU, and WebEngine.

## Runtime

The Yocto package installs the nominal conversion configuration at:

```text
/etc/monutchee/msap1/meter-conversion.json
```

At 32 kSPS the 200 ms RMS window contains exactly 6,400 frames. The daemon
opens DMA first, sends `METER_CONFIG_SET`, verifies the PL generation readback,
then requests capture START. Shutdown requests STOP before closing DMA.

Internal readers use the fixed binary `SOCK_SEQPACKET` endpoint:

```text
/run/monutchee/fpga-acquisition.sock
```

The authenticated external API is:

- `POST /api/login` and `POST /api/logout`
- `GET /api/v1/session`
- `GET /api/v1/health`
- `GET /api/v1/meter/readings`

External responses use JSON. The development login is `admin` / `admin`.
The backend owns nginx and serves HTTP 80 and HTTPS 443.

## Commands

```sh
msap1-apu-app meter-health
msap1-apu-app meter-view
msap1-apu-app meter-view --results 20
msap1-apu-app meter-view --duration 10
msap1-apu-app adc-stop
msap1-apu-app adc-start
```

`meter-health` requires matching configuration generations, responsive SPI,
active capture, zero DMA/header/FIFO errors, and advancing meter records.
`meter-view` displays mean-corrected RMS results. CH4/VLC, CH5/VLB, and CH6/VLA
are volts; current channels are intentionally zero and invalid in this stage.
