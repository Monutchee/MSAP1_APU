# MSAP1 APU ADC viewer

`msap1-apu-app` visualizes AD7771 sample data on the Linux APU without taking
ownership of the ADC hardware.

## Ownership model

- R5 core 0 is the sole owner of AD7771 SPI control, capture registers, and the
  AXI DMA S2MM channel.
- The RPU continues capturing all eight channels at 32,000 frames/s.
- The APU asks R5 core 0 for a decimated visualization copy over RPMsg. The
  default display rate is 1,000 frames/s; this does not change the ADC rate.
- The APU application never opens `/dev/spidev*`, `/dev/mem`, or a Linux DMA
  driver, so it cannot race the RPU for the ADC peripherals.

Samples are currently exported as raw, signed 24-bit ADC counts stored in
32-bit integers. Converting them to volts and amperes requires calibration and
the sensor-board analogue transfer functions.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The `meta-msap1` Yocto layer provides a `msap1-apu-app` recipe for this CMake
project and installs the package in `msap1-image`:

```bitbake
IMAGE_INSTALL:append = " msap1-apu-app"
```

## Run

Load and start the R5 core 0 firmware first. Linux also needs the `rpmsg_char`
and `rpmsg_ctrl` drivers. The viewer discovers the `mncos-r5c0-ctrl` endpoint:

```sh
msap1-apu-app adc-view
```

Useful export modes are:

```sh
msap1-apu-app adc-view --rate 1000 --format table --frames 20
msap1-apu-app adc-view --rate 1000 --format csv --output adc.csv --duration 10
msap1-apu-app adc-view --rate 1000 --format jsonl --output adc.jsonl
```

The requested rate must be no more than 4,000 frames/s and must divide the
32,000-frame/s capture rate exactly. Supported examples include 1, 10, 20, 25,
32, 40, 50, 80, 100, 125, 160, 200, 250, 320, 400, 500, 800, 1,000, 1,280,
1,600, 2,000, 3,200, and 4,000 frames/s.

For diagnostics, an already-created endpoint can be selected explicitly:

```sh
msap1-apu-app adc-view --device /dev/rpmsg0
```
