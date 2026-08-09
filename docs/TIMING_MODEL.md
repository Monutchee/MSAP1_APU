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

## The 150/180-cycle aggregate is 15 basic blocks

The first Class A aggregation tier folds exactly **15 consecutive eligible
basic blocks** into one aggregate: 150 cycles at a 50 Hz nominal, 180 at
60 Hz. Like the basic block it is cycle-defined — the nominal duration is
about 3 s, but it is NOT a 3-second timer, and the actual duration varies
with grid frequency.

- The **PL is the authoritative aggregator**, and it is not an independent
  raw-sample RMS engine: it combines the 15 standard basic results.
  Eligibility (cycle-locked, no free-run fallback, not first-after-apply,
  exact cycle count — the `class_a_aggregation_eligible` terms), same
  configuration generation, same nominal, and sample-range continuity
  across the 15 blocks are all enforced in the PL, and only complete
  15-block aggregates are emitted.
- RMS quantities aggregate per IEC 61000-4-30 as the square root of the
  arithmetic mean of the squares of the 15 basic values — unweighted, each
  basic interval contributes equally even though actual sample counts vary
  slightly: `X_agg = floor(sqrt(floor(sum(X_i^2)/15)))`, computed in the PL
  Q16 internal domain and then converted to micro-units. Frequency
  aggregates as the arithmetic mean `floor(sum(f_i)/15)` of the 15 basic
  values, valid only when all 15 were valid. That mean is **informative
  only** — the standardized Class A frequency product is defined over its
  own (10 s) interval and is not implemented yet — so the APU carries the
  value and the PL's validity flag for diagnostics
  (`AggregateTiming::frequency_valid`) but never advertises the
  `Cycles150_180` frequency reading as a valid measurement: its quality is
  always `unavailable`.
- The **APU only decodes**. MTR2 aggregate records arrive interleaved with
  basic records on the same 256-byte meter DMA stream, on an independent
  sequence counter starting at 1, so the ingestor tracks continuity per
  format. RPMsg stays control-only and DMA data-only: aggregate data never
  travels over RPMsg, and the APU never recomputes aggregate values
  (`tests/support/reference_aggregator.hpp` is a test-only reference for
  verifying the PL, not production code).
- `AggregateTiming` carries the aggregate identity — first sample index in
  the conversion domain, total sample count, the contributing basic
  sequence range — and the APU stamps TimeQuality/UTC at decode time
  exactly as for `BlockTiming`.

### Exposing the aggregate

The two record streams stay separate all the way to the API:

- The ingestor caches the newest basic record (`latest_record`, the
  instantaneous-readings source) and the newest aggregate
  (`latest_aggregate_record`) independently, with independent freshness
  clocks — the aggregate cadence is ~15x the basic one, so a stale
  aggregate must never borrow the basic record's age. The aggregate cache
  also holds the `TimeQuality` stamped onto that aggregate's decoded
  timing at ingest, because the raw 256-byte PL record carries no UTC
  state: re-decoding the cached bytes later cannot recover it. All of it
  is cleared together at the two deliberate boundaries, `begin_epoch()`
  and `clear_latest()`.
- `InfoResponse` carries both, each behind its own presence flag
  (`has_meter_record` / `has_aggregate_record`) and its own age field
  (`meter_record_age_ms` / `aggregate_record_age_ms`). Acquisition IPC
  version 18 introduced the aggregate fields; version 19 added
  `aggregate_time_quality`.
- `InfoResponse::time_quality` and `InfoResponse::aggregate_time_quality`
  answer different questions and are never interchangeable.
  `time_quality` is the timebase's state **when the reply was built** — a
  live daemon health field. `aggregate_time_quality` is the state that
  applied **when the aggregate was ingested** — the provenance of that
  measurement. An aggregate measured while synchronized and read back
  during holdover must still report `synchronized`, and regaining sync
  must never retroactively bless an older measurement.
- `GET /api/v1/meter/aggregate` (viewer role, like `/meter/readings`)
  decodes the cached MTR2 record through the shared
  `MeterDecoderRegistry` — so the endpoint inherits the decoder's identity
  validation and aggregate RMS quality rules — and renders it with the same
  channel order, naming, and units as `/meter/readings`. It answers 200 with
  `{"available": false}` whenever no aggregate exists (the first ~3 s after
  a start, ineligible basic blocks, or capture stopped); that is a normal
  state, not an error.
- The endpoint's `time_quality` field (`"unsynchronized"` |
  `"synchronized"` | `"holdover"`) is rendered from
  `InfoResponse::aggregate_time_quality`, so it describes the measurement,
  not the moment of the HTTP request. The registry decode above cannot
  supply it — the PL record holds no UTC state, so the decoded
  `AggregateTiming::time_quality` stays at its default on that path.
- The endpoint's `frequency` object is **informative only** and carries
  `"informative": true` with deliberately no validity flag, matching the
  decoder's `unavailable` quality for the `Cycles150_180` frequency
  reading. The PL's own `frequency_valid` flag stays a diagnostics-level
  detail in `AggregateTiming` and is not published as an API validity
  signal.

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

Aggregate record format `0x00020001` (MTR2) reuses the MTR1 container
(magic, 256 bytes) with its own layout: aggregate sequence (word 3), total
sample count (word 6), ANDed valid mask (word 7), status flags (word 8),
first/last contributing basic sequence (words 9–10), the composition word
(word 11: block count 15, nominal Hz, total cycle count), the 64-bit
first-sample index (words 12–13, same conversion domain as MTR1 v2 words
60–61), per-channel aggregate RMS in signed 64-bit micro-units (words
16–31, MTR1 channel order), and the mean frequency in millihertz
(word 32). Decoding produces `MeasurementPeriod::Cycles150_180` updates
carrying `AggregateTiming`; basic decoding is unchanged.

The aggregate decoder validates the record's self-declared identity before
building anything: it must be marked complete, carry a 50/60 Hz nominal,
exactly 15 basic blocks whose cycle count matches that nominal, a
first/last basic sequence span of exactly 15 consecutive blocks (modular,
so a span wrapping 0xFFFFFFFF is accepted), a non-zero sample count, and a
sample range that stays inside the 64-bit counter. Aggregate RMS quality
follows a strict priority — an aggregation arithmetic error outranks the
channel valid mask, so a saturated value can never be published as
`valid`.
