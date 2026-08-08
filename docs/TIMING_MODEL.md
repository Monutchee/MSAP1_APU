# MSAP1 measurement timing model

This document defines the Class A timing foundation shared by MSAP1_PL,
MSAP1_RPU, and MSAP1_APU. The APU types live in
`common/msap1/meter/meter_timing.hpp` and
`common/msap1/meter/measurement_timebase.hpp`.

## The basic block is cycle-defined, not time-defined

A BasicMeasurementBlock is a whole number of complete grid cycles measured
on the voltage reference channel:

- 50 Hz nominal → **10 cycles** per block
- 60 Hz nominal → **12 cycles** per block

The nominal duration is approximately 200 ms, but 200 ms is never the
semantic definition. The actual duration varies with the real grid
frequency — intentionally. A block at 49.9 Hz is longer than a block at
50.1 Hz; both are valid basic blocks. Consequently the block sample count
in an MTR1 v2 record (word 6) is the ACTUAL count, and consumers must never
assume a fixed count. When the PL loses its cycle reference it falls back
to a fixed sample window (`rms_window_samples`, derived from the nominal
frequency) and flags the block `free_run_fallback`; blocks stay gapless
across the transition.

## Two time domains

1. **Measurement time** — the PL 64-bit free-running conversion-domain
   sample counter. It increments once per accepted ADC frame from reset and
   is never reset by configuration apply, capture restart, or any clock
   correction. Block identity (`first_sample_index`, sample-range
   continuity `first(N+1) == first(N) + count(N)`) lives entirely in this
   domain. The last sample index is never transmitted: it is always
   `first + count - 1`.

2. **UTC** — a wall-clock label attached to measurement time through
   discrete sync points. The acquisition daemon periodically latches the
   sample counter together with bracketed CLOCK_REALTIME reads (via the
   waveform correlation registers) and feeds a `MeasurementTimebase`. UTC
   corrections (NTP steps, manual set) change only this mapping; no
   counter, record, or stored stream is ever rewritten.

Time quality (`Unsynchronized` → `Synchronized` → `Holdover` when the sync
point goes stale → `Synchronized` again) describes the UTC mapping only.
It must never mark the electrical measurement invalid — `TimeQuality` and
`MeasurementQuality` are separate fields by design.

## Three separate frequency concepts

| Concept | What it is | Where it lives |
|---|---|---|
| Nominal frequency | Configuration: 50 or 60 Hz, selects the cycles-per-block rule and the fallback window | settings `metering.nominal_frequency_hz`, RPMsg `nominal_frequency_hz`, MTR1 v2 timing word bits [7:0] |
| Measured frequency | The PL frequency estimator's measurement of the actual grid | MTR1 words 56–59 |
| Cycle timing | The PL zero-cross-driven block boundary machinery | PL `grid_cycle_timing`, MTR1 v2 words 6/15/60/61 |

Nominal frequency is never inferred from the measured frequency, and the
measured frequency never changes the block rule.

## Ownership

- **PL** — cycle timing (zero-cross detection, block boundaries, lock and
  fallback flags) and the monotonic 64-bit sample counter. The PL knows
  nothing about UTC.
- **RPU** — configuration conduit only: validates the nominal frequency,
  writes the PL grid registers through the shadow/apply discipline, and
  verifies readback. No MTR1 knowledge, no timing state.
- **APU** — UTC sync authority (MeasurementTimebase) and decoded data:
  record validation and continuity, BlockTiming stamping at decode time,
  durable storage, and publication.

## Record formats

MTR1 record format v2 (`0x00010002`) adds, relative to v1: actual block
sample count (word 6), the timing word (word 15: nominal Hz, cycle count,
cycle_locked / free_run_fallback / first_block_after_apply flags), and the
64-bit first-sample index (words 60–61). The v1 decoder stays registered
because stored streams may replay v1 records; v1 updates carry no
BlockTiming.
