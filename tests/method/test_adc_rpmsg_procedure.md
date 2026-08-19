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
`mnc-data`, and no Linux driver to bind AD7771 SPI/capture/config registers.
No fixed reserved-memory region is required; buffers come from DMA/CMA.

## 2. Configuration and baseline health

```sh
systemctl cat msap1-fpga-acquisition
mnc settings show
mnc meter health
```

The default command reads a concise summary from the acquisition daemon cache.
Inspect the complete counters and AD7771 register snapshot with:

```sh
mnc meter health --full
```

Verify an explicit register audit separately:

```sh
mnc meter health --refresh --full
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
complete `/data/mnc/settings/active.json` through `msap1-settings`. An invalid update must
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

## 8. Basic measurement block timing (Class A foundation)

Run with the PL raw ADC simulator selected so the grid waveform is
deterministic. The simulator frequency is set through the settings document
(`adc.simulator.frequency_hz`); the declared nominal grid frequency through
`metering.nominal_frequency_hz`.

1. Set `nominal_frequency_hz` to 60 and the simulator to 60 Hz. In
   `GET /api/v1/meter/readings` confirm the `timing` object reports
   `nominal_frequency_hz: 60`, `cycle_count: 12`, `cycle_locked: true`, and
   that `block_sequence` increments by one per record.
2. Poll several records and confirm sample-range continuity:
   `first_sample_index(N+1) = first_sample_index(N) + sample_count(N)`.
   The acquisition gap counter must not increase.
3. Offset the simulator to 59.9 Hz and 60.1 Hz. `cycle_count` must remain 12
   while `sample_count` moves above/below the nominal block length; blocks
   must remain gapless.
4. Set `nominal_frequency_hz` to 50 (simulator 50 Hz, then 49.9/50.1 Hz) and
   repeat with `cycle_count: 10`.
5. Attempt `nominal_frequency_hz: 55` through `PUT /api/v1/settings/active`;
   the request must be rejected and the active configuration unchanged.
6. Time quality: after boot with acquisition running, `time_quality` must
   progress from `unsynchronized` to `synchronized` once the periodic
   correlation sync runs. Stop `msap1-fpga-acquisition`'s sync source (or
   suspend the daemon) for longer than the staleness threshold and confirm
   `holdover`, then `synchronized` again after recovery. Meter channel
   validity must not degrade in `holdover`. `GET /api/v1/meter/aggregate`
   reports the quality captured when its aggregate was measured, so during
   these transitions it lags `/meter/readings` by up to one ~3 s aggregate
   instead of flipping with the live clock state; that is correct.
7. Disable the simulator output on CH6 (zero peak) while capturing: records
   must keep flowing with `cycle_locked: false` and `free_run_fallback:
   true`, still gapless; restoring CH6 must re-lock within one block.

## 9. Simulator fidelity features (M0 verification framework)

Prerequisite: a PL image with ADC simulator version `0x00010001`
(interpolated sine, DC offset, noise, preserve-phase, counter clears) and
the matching RPU/APU build (RPMsg wire version 3). All cases run with
`adc.source: simulator` and capture active unless stated otherwise; every
numeric expectation comes from the host golden model
(`tests/support/waveform_golden.hpp` conventions).

1. **Scenario counters.** Every simulator reconfiguration commit clears the
   simulator frame/saturation/missed counters. After
   `mnc adc simulator configure --frequency-hz 60` and a capture restart,
   `mnc adc health --full` must show the simulator counters restarting from
   zero, with `missed_sample_count` remaining 0 during nominal operation.
2. **DC offset with dc_remove.** Configure
   `mnc adc simulator configure --va-dc 5.0` (5 V offset on VA, default
   120 V RMS). With `metering.remove_dc: true`, `GET /api/v1/meter/readings`
   VA RMS must match the golden ac_rms (120 V within the golden tolerance);
   the channel mean must report ~5 V. With `remove_dc: false` the reported
   RMS must move to the golden total_rms sqrt(120^2 + 5^2) = 120.104 V.
   Restore `--va-dc 0` afterwards.
3. **Inter-channel phase accuracy.** Configure a 0.5-degree imbalance:
   `--vb-phase-degrees -120.5`. The basic records must keep all channels'
   RMS unchanged (phase never changes RMS), proving phase registers commit
   independently; full phasor verification arrives with the PhasorCore
   milestone. Restore -120 afterwards.
4. **Noise fluctuation.** Configure `--va-noise-rms 0.5` (0.5 V of
   fluctuation on VA). Poll >= 10 basic records: VA RMS must jitter from
   record to record (a bit-flat sequence is a failure), the mean of the
   reported RMS must match golden ac_rms sqrt(120^2 + 0.5^2) within the
   golden tolerance, and no other channel's jitter may change. Saturation
   count must stay 0. Restore `--va-noise-rms 0` afterwards.
5. **Preserve-phase commit.** With `--preserve-phase true`, change only
   `--va-rms` between two captures; the acquisition gap counters must stay
   zero across the stop/configure/start transaction and the first
   post-restart block must be flagged first-block exactly once. (Full
   discontinuity-free runtime events arrive with the event sequencer
   milestone; this case pins the PL contract bit end to end.)
6. **Fidelity floor.** With the default balanced configuration and no noise,
   record 30 consecutive basic records: each channel's RMS must sit inside
   the golden tolerance of its configured value, and record-to-record RMS
   spread must be below the golden quantization term - the interpolated
   sine table must not contribute visible jitter (the legacy 8-bit table
   failed this at the -48 dBc spur floor).

## 10. Single-cycle statistics (metrology M3)

Prerequisite: a matched image with SCYC-v2 records (single-cycle engine with
StatisticsCore) and the `mnc meter single-cycle` command. All cases run with
`adc.source: simulator`, capture active, cycle timing locked.

1. **Cadence and provenance.** `mnc meter single-cycle` twice, one second
   apart: `Records accepted` must advance by the nominal frequency (~60/s at
   60 Hz), `sequence` and `cycle_sequence` advance together, and
   `first_sample..last_sample` spans one cycle (sample_rate / frequency
   samples within ±1).
2. **Per-lane RMS vs golden.** With the default balanced configuration, each
   voltage lane must read its configured RMS (micro-units = volts × 1e6)
   within the golden tolerance for a 1-cycle block
   (`golden_rms_tolerance(rms, 1)`); currents likewise. The single-cycle
   values are noisier than the 12-cycle basic records — that is expected
   windowing, not error.
3. **Line-line RMS.** With balanced 120 V phases at 120° spacing, Vab/Vbc/Vca
   must read √3 × 120 V ≈ 207.85 V within tolerance. Then configure an
   unbalanced case (e.g. `--va-rms 100`): the three VLL values must match the
   golden phasor differences, NOT √3 × VLN — this is the case that proves the
   instantaneous-difference implementation.
4. **dc_remove semantics.** Add `--va-dc 5` and toggle `metering.remove_dc`:
   with removal the VA lane RMS stays at the AC value; without it the RMS
   moves to sqrt(AC² + DC²). Restore defaults afterwards.
5. **Status flag.** Normal scenarios must show `Status: 0x0`; any nonzero
   arithmetic flag at product amplitudes is a defect.

## 11. Single-cycle power (metrology M4)

Prerequisite: SCYC-v3 records (PowerCore) and the matched image. All cases via
`mnc meter single-cycle`, simulator source, capture active. Golden values from
`tests/support/waveform_golden.hpp`: P = Vrms x Irms x cos(phase_v - phase_i)
+ Vdc x Idc, per phase; import positive.

1. **Unity PF.** Balanced default (120 V / 3 A, aligned phases per pair):
   PA = PB = PC = 360 W = 360e12 pW within the golden tolerance.
2. **Displacement.** `--ia-phase-degrees -60` (voltage at 0): PA drops to
   180 W while PB/PC stay at 360 W. The handover's worked example
   (120 V, 10 A, -60 deg -> 600 W) can be run with `--ia-rms 10`.
3. **Reverse power.** `--ia-phase-degrees 180` (current fully anti-phase to
   Va at 0 deg): PA = -360 W — the record word must carry the sign (export
   negative). Restore defaults afterwards.
4. **DC power.** `--va-dc 5 --ia-dc 1` with remove_dc irrelevant to power:
   PA gains +5 W (dc_v x dc_i) on top of the AC term.
5. **Zero current.** `--ia-rms 0`: PA reads 0 within tolerance; no spurious
   power from noise (noise is uncorrelated between lanes).

## 12. Fundamental phasors and harmonic rejection (metrology M5)

Prerequisite: SCYC-v4 records (PhasorCore), simulator core 1.2 (harmonic
slots, `devmem2 0xB0080004` reads `0x00010002`), wire v4, and the matched
image. All cases via `mnc meter single-cycle` (or the single-cycle readout on
the ADC Simulator tab), simulator source, capture active, frequency
measurement enabled. Golden values from `tests/support/waveform_golden.hpp`:
`fundamental_rms` equals the configured RMS regardless of injected
harmonics; `ac_rms` grows by the harmonic energy.

1. **Pure tone baseline.** Default balanced configuration, no harmonics:
   every lane's fundamental RMS must equal its total RMS within the 1-cycle
   golden tolerance, and `Status` must show 0x0 (phasor valid).
2. **Harmonic rejection.** `mnc adc simulator configure --harmonics 3:10`
   (10% 3rd on the voltage lanes): each voltage lane's TOTAL RMS must rise
   to sqrt(1 + 0.1^2) x configured (~120.6 V at 120 V) while its
   FUNDAMENTAL RMS stays at the configured value within tolerance — the
   synchronous correlation must reject the distortion. Current lanes are
   untouched.
3. **Multi-slot.** `--harmonics 3:5,5:3,7:2` : total RMS follows the
   quadrature sum of all three slots; fundamental still unchanged. Then
   `--harmonics none` restores the pure tone.
4. **Off-nominal frequency.** `--frequency-hz 49.5` (nominal staying 50) or
   62.3 at nominal 60: fundamental RMS must stay within tolerance — theta
   comes from the MEASURED frequency, so an off-nominal grid must not leak
   fundamental energy. Restore afterwards.
5. **Invalid reference.** Stop and restart capture; the first records
   before frequency lock may report `Status` bit 1 (phasor invalid) with
   zeroed fundamental sections — the readout must say so rather than show
   stale numbers. After lock, bit 1 must clear and stay clear.
6. **Physical rule.** `--harmonics 3:10:0:all` on the balanced set: the
   3rd harmonic lands zero-sequence (identical phase on all three phases)
   by construction. Phase A active power gains
   0.01 x Vrms x Irms x cos(3 x displacement) — at aligned phases (+1%)
   this is +3.6 W on 360 W, at the -60 deg case the harmonic term is
   cos(-180 deg) = -1 so PA drops by 3.6 W. Verify against the golden
   model rather than by hand.

## 13. Single-cycle integration freeze (metrology M6)

Prerequisite: SCYC-v5 records and the matched image. All cases via
`mnc meter single-cycle` (or the web readout), simulator source, capture
active. This is the on-target golden sweep that freezes the SingleCycleResult
contract for the 10/12-cycle tier: every reported field must match
`tests/support/waveform_golden.hpp` (1-cycle tolerance,
`golden_rms_tolerance(rms, 1)`) for each scenario, and the discontinuity
semantics must hold.

1. **Whole-cycle-only emission.** Reconfigure anything (e.g. a new Va RMS):
   the first snapshot under the new generation must carry status bit 2
   (`first whole cycle after a discontinuity — reset or APPLY` in the mnc
   note) exactly once, then clear. `sample_count` must always equal
   sample_rate / frequency (a whole cycle) — never a partial count.
2. **Golden scenario sweep.** Run each scenario and compare per-lane RMS,
   VLL, per-phase P, and fundamental RMS against the golden model:
   balanced ABC; unbalanced (`--va-rms 100 --ic-rms 1`); lagging
   (`--ia-phase-degrees -60`) and leading (`+60`); reverse power (`180`);
   missing phase (`--vb-rms 0`); 50 Hz and 60 Hz nominal; off-nominal
   (e.g. `--frequency-hz 61.7`). Every field, every scenario, within
   tolerance — this is the §41 (handover) success definition restricted to
   the single-cycle tier.
3. **Sequence-gap accounting.** Over a 10-minute soak at 60 Hz the
   `Records accepted` counter must advance by ~36000 with zero acquisition
   sequence gaps and no status bits 3/4 — on a healthy system dropped-beat
   and timing-loss marks must never appear.
4. **Timing-loss recovery.** Stop capture, restart: the first snapshot
   after re-lock must carry bits 2 (and possibly 4), then run clean.
5. **Contract freeze.** After this section passes, `single_cycle_result.hpp`
   and the SCYC-v5 word map are frozen; M7's merge tier consumes them
   unchanged. Any later change requires a new format version and a
   deliberate decision.
