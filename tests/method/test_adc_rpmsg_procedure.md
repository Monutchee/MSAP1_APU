# AD7771 PL Meter Pipeline Test Procedure

The filename is retained for test-record compatibility. RPMsg carries only
configuration/control/health; meter records travel through AXI DMA.

## Prerequisites

- Deploy one coordinated PL, RPU, APU, kernel/device-tree, and Yocto build.
- Do not connect the sensor board to the grid for digital-path tests.
- Record the exact revisions and exported XSA.

## 1. Driver and ownership

```sh
dmesg | grep -E 'xilinx.*dma|msap1-meter'
ls -l /dev/msap1-meter
systemctl status msap1-fpga-acquisition --no-pager -l
```

Expect AXI DMA and `msap1_meter_dma` to probe, `/dev/msap1-meter` to belong to
`msap1-data`, and no Linux driver to bind AD7771 SPI/capture/config registers.
No fixed reserved-memory region is required; buffers come from DMA/CMA.

## 2. Configuration and baseline health

```sh
cat /etc/monutchee/msap1/meter-conversion.json
msap1-apu-app meter-health
```

Expect `PASS`, 32,000 frame/s, a 6,400-frame RMS window, matching non-zero
configuration generations, active capture, and zero DMA read, invalid-record,
sequence-gap, FIFO-overflow, and header-error counts.

## 3. Meter result rate and content

```sh
msap1-apu-app meter-view --results 50
```

Expect 50 strictly advancing records in about ten seconds (5 Hz). Every DMA
record is exactly 256 bytes and contains `MTR1`; CH4–CH6 are valid RMS volts.
CH0–CH3 remain zero/invalid. Packetizer/hub drop counters remain zero.

## 4. Concurrent readers

Run `meter-view` while the authenticated web page or
`GET /api/v1/meter/readings` polls concurrently. Both must observe advancing
snapshots without stealing records, blocking acquisition, or disrupting the
RPU heartbeat and health endpoint.

## 5. Lifecycle and sustained run

Repeat ten times:

```sh
msap1-apu-app adc-stop
msap1-apu-app adc-start
msap1-apu-app meter-health
```

Then run for at least ten minutes. Expect no DMA errors, sequence gaps, corrupt
records, PL header/FIFO errors, RPMsg timeouts, or heartbeat starvation. Confirm
that no raw ADC samples or meter records travel over RPMsg.
