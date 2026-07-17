# AD7771 Linux Acquisition Test Procedure

The filename is retained for test-record compatibility. Sample data no longer
travels over RPMsg: Linux receives it through IIO/DMAengine, while RPMsg carries
only ADC capture control and health.

## Prerequisites and safety

- Do not connect the sensor board to the electrical grid for digital-path tests.
- Deploy one coordinated build containing the SG-enabled PL, protocol-v2 RPU,
  IIO kernel driver/device tree, acquisition daemon, and APU client.
- Record the PL, RPU, APU, meta-monutchee, and manifest revisions.
- Confirm `msap1-fpga-acquisition.service` is active and the RPU heartbeat is
  blinking.

## Command reference

```sh
systemctl status msap1-fpga-acquisition
msap1-apu-app adc-health
msap1-apu-app adc-view --rate RATE --channels "4,5,6" --duration SECONDS
msap1-apu-app adc-stop
msap1-apu-app adc-start
```

`--rate` is per-reader display decimation. It must divide 32,000 exactly and
does not alter the ADC, DMA, IIO, shared-ring, or another reader's rate.

| View rate | Cursor stride | Expected 10-second output | Acquisition rate |
| ---: | ---: | ---: | ---: |
| 20 frame/s | 1,600 | about 200 frames | 32,000 frame/s |
| 1,000 frame/s | 32 | about 10,000 frames | 32,000 frame/s |
| 32,000 frame/s | 1 | about 320,000 frames | 32,000 frame/s |

## 1. Driver and overlay probe

```sh
dmesg | grep -E 'xilinx.*dma|msap1-ad7771|iio'
cat /sys/bus/iio/devices/iio:device*/name
```

Expected results:

- Xilinx AXI DMA probes with its S2MM channel.
- one IIO name is `msap1-ad7771` and `/dev/iio:deviceX` exists;
- no Linux driver binds the AD7771 AXI SPI or PL capture-register node;
- no fixed ADC reserved-memory region is present.

## 2. Baseline health and throughput

Run `msap1-apu-app adc-health` twice, at least five seconds apart. Expect:

- overall result `PASS`;
- Linux acquisition, SPI response, initialization, configuration, and capture
  are healthy;
- IIO frames, bytes, DMA blocks, PL frames, and PL packets increase;
- IIO read errors, FIFO overflows, and header errors remain zero;
- sustained IIO throughput is approximately 1,024,000 bytes/s: 32,000 frames/s
  times eight 32-bit storage words;
- each PL packet is 256 ordered frames, 8,192 bytes, terminated by `TLAST`.

ADC alert counts may rise with open or out-of-common-mode analogue inputs and
do not by themselves identify a DOUT framing error.

## 3. Independent-reader test

Run these in separate shells at the same time:

```sh
msap1-apu-app adc-view --rate 20 --channels "4,5,6" --duration 30
msap1-apu-app adc-view --rate 1000 --format csv --output /tmp/adc-second.csv --duration 30
```

Both commands must finish without stealing data from each other, stopping
capture, blocking the daemon, or disrupting the heartbeat/RPMsg health path.
A reader that falls more than 262,144 frames behind may report its own
shared-ring overrun; acquisition and other readers must continue.

## 4. Repeated lifecycle test

Repeat at least ten times:

```sh
msap1-apu-app adc-stop
msap1-apu-app adc-start
msap1-apu-app adc-health
```

STOP must disable/reset PL capture before Linux releases DMA. START must arm
IIO/DMA before enabling PL capture. The RPU heartbeat and RPMsg health command
must remain responsive throughout.

## 5. Sustained run

Run acquisition for at least ten minutes while periodically checking health
and running two viewers. Expect no DMA/IIO errors, sequence gaps, FIFO/header
errors, shared-ring corruption, or RPU starvation. Confirm with RPMsg tracing or
protocol counters that no ADC sample payloads cross RPMsg.

## Test record

Record exact revisions and commands, elapsed time, frame/byte/block deltas,
calculated throughput, viewer overruns, health before/after, heartbeat state,
and any DMA, IIO, FIFO, header, sequence, or RPMsg errors.
