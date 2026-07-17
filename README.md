# MSAP1 Linux FPGA acquisition

`msap1-fpga-acquisition` owns the Linux AD7771 data path. It reads the eight
channel IIO stream backed by AXI DMA and publishes samples for independent
Linux consumers. `msap1-apu-app` is the diagnostic viewer and health client.

## Ownership model

- R5 core 0 owns AD7771 SPI configuration, reset/synchronization, PL capture
  control, and health.
- Linux owns AXI DMA S2MM, scatter-gather descriptors, interrupts, and
  CMA-backed DDR buffers through `msap1_ad7771_iio`.
- The daemon is the only process that opens `/dev/iio:deviceX` and the R5 core
  0 RPMsg endpoint. RPMsg carries START, STOP, and health messages only.
- The daemon writes all 32,000 frames/s to an 8 MiB POSIX shared-memory ring.
  Each reader has an independent cursor, so one lagging client cannot steal or
  block another client's data.

Samples are currently exported as raw, signed 24-bit ADC counts stored in
32-bit integers. Converting them to volts and amperes requires calibration and
the sensor-board analogue transfer functions.

## Build

Initialize the Linux OpenAMP helper submodule after cloning the repository:

```sh
git submodule update --init --recursive
```

The daemon uses `mnc::RpmsgChrdev` from that helper for RPMsg service discovery,
`rpmsg_chrdev` binding, and endpoint I/O.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The `meta-msap1` Yocto layer provides a `msap1-apu-app` recipe for this CMake
project and installs the package in `msap1-image`. Git-based Yocto fetches must
use the `gitsm://` fetcher so the helper submodule is present:

```bitbake
IMAGE_INSTALL:append = " msap1-apu-app"
```

## Runtime architecture

The Yocto package enables `msap1-fpga-acquisition.service`. Its startup order is
IIO scan setup, buffer/DMA enable, then RPU capture START. Shutdown reverses the
control edge: RPU STOP, IIO disable, then DMA release.

The default endpoints are:

- IIO device name: `msap1-ad7771`
- control socket: `/run/msap1/fpga-acquisition.sock`
- shared memory: `/msap1-fpga-acquisition`
- ring capacity: 262,144 frames (8 MiB, about 8.2 seconds at 32 kSPS)

Inspect the service and combined Linux/RPU health:

```sh
systemctl status msap1-fpga-acquisition
msap1-apu-app adc-health
```

Control acquisition explicitly when needed:

```sh
msap1-apu-app adc-stop
msap1-apu-app adc-start
```

## View and export

Read a decimated view from the shared ring:

```sh
msap1-apu-app adc-view
```

Show only the three voltage channels (AD7771 AIN4/AIN5/AIN6):

```sh
msap1-apu-app adc-view --channels 4,5,6
```

Useful export modes are:

```sh
msap1-apu-app adc-view --rate 1000 --format table --frames 20
msap1-apu-app adc-view --rate 1000 --format csv --output adc.csv --duration 10
msap1-apu-app adc-view --rate 1000 --format jsonl --output adc.jsonl
```

`adc-health` exits successfully only when Linux acquisition is running, IIO has
published data without read errors, SPI/configuration health passes, capture is
active, and the PL reports neither FIFO overflow nor channel-header errors.

The requested view rate must be no greater than the capture rate and must
divide 32,000 exactly. It affects only this reader's cursor; DMA, IIO, the
shared ring, and other readers continue at the full ADC rate.
