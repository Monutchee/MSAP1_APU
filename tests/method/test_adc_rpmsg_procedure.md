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
systemctl cat msap1-fpga-acquisition
cat /etc/monutchee/msap1/default/adc_config/msap1-sensor-board-5a.json
mnc meter health
```

Expect `PASS`, 32,000 frame/s, a 6,400-frame RMS window, matching non-zero
configuration generations, active capture, and zero DMA read, invalid-record,
sequence-gap, FIFO-overflow, and header-error counts. The service command must
select the complete 5 A profile, and ADC configuration health must confirm the
programmed PGA readback.

To compare mean-corrected AC RMS with zero-referenced total RMS, set
`remove_dc` to `true` or `false` respectively, then apply it without rebooting:

```sh
systemctl restart msap1-fpga-acquisition
mnc meter health
mnc meter view --results 10
```

Expect health to remain `PASS` in either mode and `DC offset removal` to report
the selected setting. Restore `remove_dc` to `true` after the diagnostic test.

## 3. Meter result rate and content

```sh
mnc meter view --results 50
```

Expect 50 strictly advancing records in about ten seconds (5 Hz). Every DMA
record is exactly 256 bytes and contains `MTR1`; CH0–CH3 are valid RMS amps
with the 5 A profile and CH4–CH6 are valid RMS volts. Packetizer/hub drop
counters remain zero.

With an isolated reference source on VLA, test 40, 50, 60, and 70 Hz. The
displayed frequency must settle after the configured complete-cycle interval,
remain within ±0.05 Hz of the source, and become `unavailable` after the
three-period no-signal timeout when VLA is removed. Positive/negative DC offset
and changing RMS `remove_dc` must not change the measured frequency.

As an authenticated administrator, exercise:

```text
GET /api/v1/meter/configuration/frequency
PUT /api/v1/meter/configuration/frequency
```

Verify single-cycle, rolling-cycle, and rolling-time modes. A valid update must
stop, apply, restart, change the configuration generation, and create the
complete `/etc/monutchee/msap1/adc_config/active.json`. An invalid update must
return a client error, retain the prior profile, and restore capture.

## 4. Concurrent readers

Run `mnc meter view` while the authenticated web page or
`GET /api/v1/meter/readings` polls concurrently. Both must observe advancing
snapshots without stealing records, blocking acquisition, or disrupting the
RPU heartbeat. Confirm `GET /api/v1/meter/health` agrees with
`mnc meter health` while the system remains healthy.

The health output must also show an advancing ADC packet count and a nonzero
ADC DCLK measurement after approximately two seconds. Compare the reported
DCLK with the clock selected by the AD7771 output configuration. Stopping the
ADC clock must make the field unavailable after the next observation window;
restoring the clock must recover without restarting Linux.

## 5. Lifecycle and sustained run

Repeat ten times:

```sh
mnc adc stop
mnc adc start
mnc meter health
```

Using an authenticated admin session, repeat the lifecycle with
`DELETE /api/v1/adc/capture` and `PUT /api/v1/adc/capture`. Each response must
report the resulting `active` state, repeated requests must be idempotent, and
`GET /api/v1/adc/capture` must agree with the CLI.

Then run for at least ten minutes. Expect no DMA errors, sequence gaps, corrupt
records, PL header/FIFO errors, RPMsg timeouts, or heartbeat starvation. Confirm
that no raw ADC samples or meter records travel over RPMsg.
