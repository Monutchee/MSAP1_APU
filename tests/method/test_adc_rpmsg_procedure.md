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

The default command reads the acquisition daemon cache. Verify an explicit
register audit separately:

```sh
mnc meter health --refresh
```

The output should show a meter-record age below 1000 ms, no pending health
confirmation, and stable SPI protocol/retry counters. A single recovered retry
may increment the diagnostic counters without degrading the cached health.

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
ADC DCLK and DRDY measurements after approximately two seconds. Compare the
reported DCLK with the clock selected by the AD7771 output configuration and
compare DRDY with the configured ADC frame rate. Stopping the ADC clock must
make both fields unavailable after the next observation window; restoring it
must recover without restarting Linux.

Stopping capture does not remove the raw AD7771 register snapshot. Use its decoded
power mode, filter, DOUT topology, reference mux, and SRC-derived ODR to compare
the requested configuration with the physical DRDY measurement. The snapshot
also includes per-channel PGA/configuration, phase offsets, digital
offset/gain calibration, and detailed error/status registers.

## 5. Runtime sample-rate diagnostic

The rate command safely stops DMA/capture, reloads the AD7771 SRC, recalculates
the PL RMS and frequency window sample counts, and restores the prior running
state. Run:

```sh
mnc adc rate --sps 16000
mnc adc rate --sps 32000
mnc adc rate --sps 64000
```

Expect:

| Requested | SRC integer N | Physical DRDY |
| ---: | ---: | ---: |
| 16,000 | 128 | approximately 16 kframe/s |
| 32,000 | 64 | approximately 32 kframe/s |
| 64,000 | 32 | approximately 64 kframe/s |

Each command waits for two consecutive one-second DRDY measurements to agree,
then prints requested, SRC-derived, DCLK, and measured DRDY rates.
A configuration transaction that succeeds but produces an unexpected physical
DRDY rate prints `MISMATCH` without turning the CLI transaction itself into an
error. Header/FIFO/DMA errors must remain zero, configuration generation must
change with the rate, and meter results must remain five records per second
because the 200 ms window duration is preserved.

If DRDY follows the three requested values, the active SRC accepted every
explicit load. If SRC register-derived rates change but DRDY remains fixed,
the active DSP decimator or physical ADC control path requires investigation.
Restore the normal operating point:

```sh
mnc adc rate --sps 32000
mnc meter health
```

The selection is temporary. Restarting `msap1-fpga-acquisition` must restore
the packaged 32 kSPS default.

## 6. Warm-reset and SRC-load diagnostic

When SRC holding registers imply one rate but physical DRDY reports another,
run:

```sh
mnc adc testflw --flow 1
```

The command preserves the daemon's prior running/stopped state and prints four
snapshots:

1. Active state before reset.
2. DCLK/DRDY while PL holds the sensor-board `ADC_RESET_N` output low for
   2.2 seconds.
3. SPI reset defaults immediately after `INIT_COMPLETE`, before configuration.
4. Active registers and rates after a conservative 1 ms
   `SRC_UPDATE=1` pulse, readback high/low, filter synchronization, and a fresh
   PL measurement window.

This is not an independent cold reset or power cycle. Linux and the FPGA remain
running; only the existing ADC reset pin driven through the capture core is
pulsed. Copy the complete output when reporting the result.

Expected diagnostic control checks are:

```text
RESET_N commanded       yes
DRDY stopped in reset   yes
Reset defaults read     yes
SRC_UPDATE read high    yes (0x01)
SRC_UPDATE read low     yes (0x00)
SRC holding match       yes
Final config match      yes
```

`Final DRDY match` should be `yes` after the hardware START synchronization
fix. A `no` result means reset/register loading completed but the ADC output
rate remains incorrect. A non-`none` flow error or failure stage means the
diagnostic itself did not complete. After the command, confirm the prior
acquisition state and heartbeat were restored:

```sh
mnc meter health
```

## 7. Lifecycle and sustained run

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
