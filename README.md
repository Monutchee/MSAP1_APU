# MSAP1 Linux meter acquisition

`msap1-fpga-acquisition` is the sole Linux owner of meter-result DMA and the
R5 core 0 RPMsg control endpoint. It converts the product JSON configuration
to fixed-point PL coefficients, commits them through the RPU, starts capture,
and caches fixed 256-byte `MTR1` records from `/dev/msap1-meter`.

The data path is:

```text
AD7771 capture -> PL conversion -> PL RMS + VLA frequency -> AXI DMA
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

The Yocto package installs complete schema-version-2 ADC profiles at:

```text
/etc/monutchee/msap1/default/adc_config/acuvim3-sb-1a.json
/etc/monutchee/msap1/default/adc_config/acuvim3-sb-5a.json
/etc/monutchee/msap1/default/adc_config/acuvim3-sb-mv.json
```

Each profile also contains the CH6/VLA positive zero-crossing frequency
configuration. Older schema-version-2 profiles without that object receive
the rolling-10-cycle, 40–70 Hz, 1 V hysteresis defaults. Runtime frequency
limits must remain ordered within the supported 10–200 Hz range.

The systemd service selects `acuvim3-sb-5a.json` by default. Pass exactly one
complete profile with `msap1-fpga-acquisition --config <path>`; files are not
merged. A valid `/etc/monutchee/msap1/adc_config/active.json` takes precedence
at boot. An authenticated frequency update stages and validates the complete
profile, restarts the coordinated RPU/PL capture transaction, and atomically
persists that complete active profile. The package never overwrites it.

`mnc meter-view` displays the user-facing meter channels CH0 through CH6.
CH7/VCM remains available in the MTR1 record and internal API model for future
reference-monitoring support, but is intentionally hidden from the CLI.

At 32 kSPS the 200 ms RMS window contains exactly 6,400 frames. The daemon
opens DMA first, stops capture, programs and verifies each AD7771 channel PGA
through the RPU, sends the derived PL coefficients, verifies the PL generation
readback, then requests capture START. Shutdown requests STOP before closing
DMA.

`remove_dc` selects the PL RMS reference. It defaults to `true`, producing AC
RMS about the window mean. Set it to `false` for total RMS referenced to zero,
then restart `msap1-fpga-acquisition` to commit the updated configuration.

Internal readers use the fixed binary `SOCK_SEQPACKET` endpoint:

```text
/run/monutchee/fpga-acquisition.sock
```

The authenticated external API is:

- `POST /api/login` and `POST /api/logout`
- `GET /api/v1/session`
- `GET /api/v1/health`
- `GET /api/v1/meter/health`
- `GET /api/v1/meter/readings`
- `GET /api/v1/meter/configuration/frequency`
- `PUT /api/v1/meter/configuration/frequency`
- `GET /api/v1/adc/capture`
- `PUT /api/v1/adc/capture` and `DELETE /api/v1/adc/capture`

External responses use JSON. The development login is `admin` / `admin`.
The backend owns nginx and serves HTTP 80 and HTTPS 443. Read-only routes
require the viewer role; changing ADC capture requires the admin role.

## Commands

```sh
mnc meter health
mnc meter view
mnc meter view --results 20
mnc meter view --duration 10
mnc adc stop
mnc adc start
```

`mnc meter health` reports both the number of AXI packets accepted by the
capture stream, external AD7771 DCLK frequency, and `ADC_DRDY_N` frame rate
measured in PL. The DCLK and DRDY fields are `unavailable` until two one-second
observation windows complete, and whenever the corresponding signal stops.

Run `mnc`, `mnc help meter`, or append `--help` to any command for contextual
help. Global `--socket` and `--timeout-ms` options are accepted before or after
the command path. The Yocto package installs Bash completion for command groups,
actions, options, and socket paths.

`mnc meter health` requires matching configuration generations, responsive SPI,
active capture, zero DMA/header/FIFO errors, and advancing meter records.
`mnc meter view` displays RMS results and the latest VLA grid frequency. An
absent/out-of-range grid is reported as unavailable without failing acquisition;
a frequency arithmetic fault does fail meter health. RMS uses the configured
`remove_dc` mode, while frequency always uses actual signal zero crossings.
CH0/ILA through CH3/ILN are amps when the selected current-sensor definition is
enabled; CH4/VLC through CH6/VLA are volts. The packaged mV profile deliberately
leaves current invalid until a complete customized profile supplies the
sensor's primary-current rating.
