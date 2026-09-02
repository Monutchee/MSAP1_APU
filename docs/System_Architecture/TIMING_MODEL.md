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
in a BASIC-v4 record (word 6) is the ACTUAL count, and consumers must never
assume a fixed count. When the PL loses its cycle reference it falls back
to a fixed sample window (`rms_window_samples`, derived from the nominal
frequency) and flags the block `free_run_fallback`; blocks stay gapless
across the transition.

R5C1 assembles the cycle results into Basic windows. At every programmed UTC
ten-minute sample target, the first whole cycle whose first sample is at or
after the target starts the synchronized cadence. If an old Basic window is
open, that cycle is folded into both the continuing old slot and one transient
shadow slot; the old window closes normally and the selector promotes the
shadow without copying its accumulator image. The first Basic result on the
new cadence carries timing-word bit 19 (`utc_resynchronized`). Consecutive
Basic records may therefore overlap only at this marked transition; the APU
accepts a marked lateral overlap but still rejects an unmarked overlap,
contained duplicate, or forward gap.

## The 150/180-cycle aggregate is 15 basic blocks

The first Class A aggregation tier folds exactly **15 consecutive eligible
basic blocks** into one aggregate: 150 cycles at a 50 Hz nominal, 180 at
60 Hz. Like the basic block it is cycle-defined — the nominal duration is
about 3 s, but it is NOT a 3-second timer, and the actual duration varies
with grid frequency.

- **R5C1 is the authoritative aggregator**, and it is not an independent
  raw-sample RMS engine: it combines the merge-safe sufficient statistics of
  15 standard Basic results and reuses the shared interval finalizer.
  Eligibility (cycle-locked, no free-run fallback, not first-after-apply,
  exact cycle count — the `class_a_aggregation_eligible` terms), same
  configuration generation, same nominal, and sample-range continuity (with
  one explicitly marked lateral UTC transition) across the 15 blocks are all
  enforced in R5C1, and only complete 15-block aggregates are emitted.
- Electrical quantities are finalized from the summed accumulator images;
  previously rounded Basic RMS values are never averaged. Frequency
  aggregates as the arithmetic mean `floor(sum(f_i)/15)` of the 15 basic
  values, valid only when all 15 were valid. That mean is **informative
  only** — the standardized Class A frequency product is defined over its
  own (10 s) interval and is not implemented yet — so the APU carries the
  value and the PL's validity flag for diagnostics
  (`AggregateTiming::frequency_valid`) but never advertises the
  `Cycles150_180` frequency reading as a valid measurement: its quality is
  always `unavailable`.
- The **APU only decodes**. AGG-v3 aggregate records arrive interleaved with
  basic records on the same 256-byte meter DMA stream, on an independent
  sequence counter starting at 1, so the ingestor tracks continuity per
  format. RPMsg stays control-only and DMA data-only: aggregate data never
  travels over RPMsg, and the APU never recomputes aggregate values
  (`tests/support/reference_aggregator.hpp` is a test-only reference for
  verifying the PL, not production code).
- At a UTC transition, a synchronized Basic seeds a second 150/180-cycle
  slot while also completing the pre-boundary interval. Both intervals still
  contain exactly 15 consecutive Basic sequence numbers. The continuing
  record carries status bit 3 (`utc_overlap`); the new record carries bit 4
  (`utc_resynchronized`). Only the continuing record may have an actual
  first-to-last sample span shorter than its summed contribution count. If the
  UTC target lands exactly on a Basic boundary, the continuing record remains
  marked as the member that overlaps the synchronized aggregate, but its own
  contribution span remains contiguous.
- `AggregateTiming` carries the aggregate identity — first and actual last
  sample indices, total contribution count, contributing Basic sequence
  range, and UTC-overlap provenance — and the APU stamps TimeQuality/UTC at
  decode time exactly as for `BlockTiming`.

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
  decodes the cached AGG-v3 record through the shared
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
   domain. BASIC-v4 transmits the actual last sample in words 14–15. AGG-v3
   transmits it in words 36–37 because a continuing UTC-overlap aggregate has
   `last < first + count - 1`; all non-overlap ranges retain the usual
   `last == first + count - 1` invariant.

2. **UTC** — a wall-clock label attached to measurement time through
   discrete sync points. The acquisition daemon periodically latches the
   sample counter together with kernel-bracketed CLOCK_REALTIME reads through
   the independent, register-only `/dev/meter-time` endpoint and feeds a
   `MeasurementTimebase`. UTC
   corrections (NTP steps, manual set) change only this mapping; no
   counter, record, or stored stream is ever rewritten.

Time quality (`Unsynchronized` → `Synchronized` → `Holdover` when the sync
point goes stale → `Synchronized` again) describes the UTC mapping only.
It must never mark the electrical measurement invalid — `TimeQuality` and
`MeasurementQuality` are separate fields by design.

## Three separate frequency concepts

| Concept | What it is | Where it lives |
|---|---|---|
| Nominal frequency | Configuration: 50 or 60 Hz, selects the cycles-per-block rule and the fallback window | settings `metering.nominal_frequency_hz`, RPMsg `nominal_frequency_hz`, BASIC-v4 timing word bits [7:0] |
| Measured frequency | The PL frequency estimator's measurement of the actual grid | BASIC-v4 words 56–59 |
| Cycle timing | The PL zero-cross-driven block boundary machinery | PL `grid_cycle_timing`, BASIC-v4 words 6/9/10/13 |

Nominal frequency is never inferred from the measured frequency, and the
measured frequency never changes the block rule.

## Ownership

- **PL** — cycle timing (zero-cross detection, cycle boundaries, lock and
  fallback flags), merge-safe single-cycle statistics, and the monotonic
  64-bit sample counter. The PL exports the Linux-programmed sample-domain UTC
  target as context but does not perform wall-clock conversion.
- **RPU** — R5C1 owns Basic and longer interval aggregation, including the
  two-slot UTC transition state and complete Basic/aggregate serialization. R5C0
  remains the configuration conduit for grid timing and capture control.
- **APU** — UTC sync authority (MeasurementTimebase) and decoded data:
  record validation and continuity, BlockTiming stamping at decode time,
  durable storage, and publication.

## Record formats

Both formats share one envelope in words 0..12 (normative source:
MSAP1_PL `SourceData/HLS_DesignFile/common/include/measurement_record.hpp`;
the PL engines build and serialize their own records): magic, format,
size 256, per-producer sequence (word 3), configuration generation (4),
sample rate (5), actual sample count (6), valid mask (7), status (8), the
64-bit first-sample index (words 9–10, the PL conversion-domain counter),
and the transport drop words (11–12, constant 0 by construction).

The current Basic format v4 (`0x00010004`) adds the timing word (word 13:
nominal Hz, cycle count, cycle_locked / free_run_fallback /
first_block_after_apply flags, plus bit 19 `utc_resynchronized`), the actual
last-sample index (words 14–15), per-channel readings (words 16–55), the
frequency block (56–59), and capture diagnostics (60–63).

Aggregate format v3 `0x00020003` (AGG-v3) carries the composition word
(word 13: block count 15, nominal Hz, total cycle count), the first/last
contributing basic sequence (words 14–15), per-channel aggregate RMS in
signed 64-bit micro-units (words 16–31, basic-record channel order), the mean
frequency in millihertz (word 32), and the aggregation-engine diagnostics
as of the emit (words 33–35: reset / ineligible / continuity counts). Words
36–37 carry the actual last sample. Status bits 3 and 4 distinguish the
continuing overlap interval from the newly synchronized interval.
Decoding produces `MeasurementPeriod::Cycles150_180` updates carrying
`AggregateTiming`; basic decoding is unchanged. Earlier formats (v1
`0x00010001`, v2 `0x00010002`, aggregate v1 `0x00020001`) are not decodable —
PL and APU ship together, and pre-production stores were reset at the
cutover.

The aggregate decoder validates the record's self-declared identity before
building anything: it must be marked complete, carry a 50/60 Hz nominal,
exactly 15 basic blocks whose cycle count matches that nominal, a
first/last basic sequence span of exactly 15 consecutive blocks (modular,
so a span wrapping 0xFFFFFFFF is accepted), a non-zero sample count, and a
sample range that stays inside the 64-bit counter. An unmarked or synchronized
record must have `actual_last == first + count - 1`; an overlap record may
have either that contiguous span (exact-boundary alignment) or a shorter,
nonempty physical span, and conflicting provenance bits are rejected.
Aggregate RMS quality
follows a strict priority — an aggregation arithmetic error outranks the
channel valid mask, so a saturated value can never be published as
`valid`.
