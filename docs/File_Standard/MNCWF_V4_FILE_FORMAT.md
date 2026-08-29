# MNCWF version 4 file format

## Status

This is the normative byte contract for a file whose magic is `MNCWF1\0\0`
and whose version is `4`. The production encoder and defensive reader are
[`mncwf_v4.hpp`](../../common/msap1/waveform/mncwf_v4.hpp) and
[`mncwf_v4.cpp`](../../common/msap1/waveform/mncwf_v4.cpp). Its synthetic and
adversarial vectors are in
[`mncwf_v4_reader_test.cpp`](../../tests/mncwf_v4_reader_test.cpp).

The acquisition daemon writes MNCWF v4 for new captures. It freezes settings,
device/channel identity, time quality, event descriptors, and UUID lineage at
capture time, validates the encoded bytes through the independent reader, and
publishes the file only after an atomic temporary-file rename. Existing v1-v3
files remain discoverable as legacy history.

MNCWF v4 is the only planned on-device waveform master. COMTRADE and PQDIF are
future export conversions; they do not replace or supplement this recording
format. See
[`MNCWF_V4_CONVERSION_READINESS.md`](MNCWF_V4_CONVERSION_READINESS.md).

## Binary conventions and limits

- Every integer is explicitly little-endian. Signed integers are two's
  complement.
- The file has no native-structure padding. Readers must decode fields rather
  than cast file bytes to C or C++ structures.
- Header and directory offsets are absolute from the start of the file.
  String references are relative to their containing section's blob.
- Each section starts on an eight-byte boundary. Zero bytes pad between
  sections; padding is not included in the preceding section's CRC.
- Reserved fields, reserved bytes, external alignment padding, and unknown
  flag bits are zero.
- Text is strict UTF-8 without embedded NUL bytes. One referenced string is at
  most 64 KiB.
- A reader validates the complete file before exposing a sample span.
- The implemented defensive limits are 512 MiB/file, 64 sections, 64
  channels, 4096 events, 4096 lineage entries, 65536 timebase segments, and
  16 MiB for each non-sample section.
- Version 4 does not define compression or encryption. `logical_bytes` must
  equal `stored_bytes`.

All offsets, lengths, counts, additions, multiplications, rational
denominators, and host-size conversions are untrusted input.

## Top-level layout

```text
+-------------------------------+ offset 0
| 64-byte v4 header             |
+-------------------------------+ offset 64
| section_count * 56 directory  |
+-------------------------------+ next 8-byte boundary
| section payload               |
| zero alignment padding        |
| ...                           |
+-------------------------------+ exact declared file size
```

There are seven mandatory section types. A writer may order sections
arbitrarily, but their byte extents must form an exact, non-overlapping cover
of the bytes after the directory, apart from zero alignment padding.

| Type | Name | Section version | Mandatory |
|---:|---|---:|---|
| 1 | capture metadata | 1 | yes |
| 2 | timebase segments | 1 | yes |
| 3 | channel definitions | 1 | yes |
| 4 | event descriptors | 1 | yes |
| 5 | quality intervals | 1 | yes |
| 6 | lineage | 1 | yes |
| 7 | sample data | 1 | yes |

An unknown section with directory flag bit 0 clear is optional and is skipped
after its extent and CRC have been validated. An unknown section with bit 0
set is required and makes the file unsupported.

## Header

The header is exactly 64 bytes:

| Offset | Size | Type | Field |
|---:|---:|---|---|
| 0 | 8 | bytes | `MNCWF1\0\0` |
| 8 | 4 | `u32` | version, exactly `4` |
| 12 | 4 | `u32` | header bytes, exactly `64` |
| 16 | 4 | `u32` | directory entry bytes, exactly `56` |
| 20 | 4 | `u32` | section count |
| 24 | 8 | `u64` | directory offset, exactly `64` |
| 32 | 8 | `u64` | directory bytes, exactly `section_count * 56` |
| 40 | 8 | `u64` | exact file bytes |
| 48 | 4 | `u32` | header flags, currently zero |
| 52 | 4 | `u32` | directory CRC32C |
| 56 | 4 | `u32` | header CRC32C |
| 60 | 4 | `u32` | reserved, zero |

The directory CRC covers the exact directory bytes. The header CRC covers all
64 header bytes with bytes 56 through 59 temporarily zero; it therefore also
protects the stored directory CRC. Writers calculate the directory CRC first,
then the header CRC.

CRC32C uses the reflected Castagnoli polynomial `0x82F63B78`, initial value
`0xFFFFFFFF`, and final XOR `0xFFFFFFFF`. The check value for ASCII
`123456789` is `0xE3069283`.

## Directory entry

Every directory entry is exactly 56 bytes:

| Offset | Size | Type | Field |
|---:|---:|---|---|
| 0 | 4 | `u32` | section type; zero is invalid |
| 4 | 2 | `u16` | section version; zero is invalid |
| 6 | 2 | `u16` | flags; bit 0 means required |
| 8 | 8 | `u64` | aligned absolute payload offset |
| 16 | 8 | `u64` | stored payload bytes, nonzero |
| 24 | 8 | `u64` | logical bytes, equal to stored bytes |
| 32 | 8 | `u64` | item count |
| 40 | 4 | `u32` | bytes per fixed record |
| 44 | 4 | `u32` | CRC32C of exact stored payload bytes |
| 48 | 8 | `u64` | reserved, zero |

Each known section appears exactly once and has the required flag set. A
duplicate known section, a known optional section, or a missing known section
is invalid.

## Known-section envelope

Each known section begins with the same 48-byte envelope. Unknown optional
sections define their own payload shape.

| Offset | Size | Type | Field |
|---:|---:|---|---|
| 0 | 4 | `u32` | section type, equal to directory |
| 4 | 2 | `u16` | section version, equal to directory |
| 6 | 2 | `u16` | envelope bytes, exactly `48` |
| 8 | 4 | `u32` | envelope flags, currently zero |
| 12 | 4 | `u32` | record bytes, equal to directory `item_bytes` |
| 16 | 8 | `u64` | record count, equal to directory `item_count` |
| 24 | 8 | `u64` | blob offset, or zero when there is no blob |
| 32 | 8 | `u64` | blob bytes, or zero |
| 40 | 8 | `u64` | reserved, zero |

Fixed records immediately follow the envelope. A present blob begins at the
next eight-byte boundary after the records and ends at the exact end of the
section. Its internal alignment padding is zero. Sections that do not permit
a blob have no trailing bytes.

A string reference is `{offset:u32, bytes:u32}`. `{0,0}` is the canonical
empty string. References cannot escape or wrap the section blob.

## Capture metadata section

This section contains exactly one 256-byte record and a UTF-8 blob.

| Offset | Size | Type | Field |
|---:|---:|---|---|
| 0 | 16 | bytes | stable capture UUID |
| 16 | 16 | bytes | stable device UUID |
| 32 | 32 | bytes | SHA-256 of complete capture-time configuration |
| 64 | 32 | bytes | SHA-256 of sensor profile |
| 96 | 8 | `u64` | file creation TAI nanoseconds; zero if unavailable |
| 104 | 8 | `u64` | file creation UTC nanoseconds; zero if unavailable |
| 112 | 8 | `s64` | nominal voltage numerator, SI volts |
| 120 | 8 | `u64` | nominal voltage denominator, nonzero |
| 128 | 8 | `u64` | nominal frequency numerator, hertz |
| 136 | 8 | `u64` | nominal frequency denominator, nonzero |
| 144 | 4 | `u32` | topology |
| 148 | 4 | `u32` | calibration status |
| 152 | 4 | `u32` | flags, currently zero |
| 156 | 4 | `u32` | reserved, zero |
| 160 | 8 | ref | station name |
| 168 | 8 | ref | site/location name |
| 176 | 8 | ref | circuit/feeder name |
| 184 | 8 | ref | product name |
| 192 | 8 | ref | device model |
| 200 | 8 | ref | firmware version |
| 208 | 8 | ref | software build ID |
| 216 | 8 | ref | sensor-profile ID |
| 224 | 8 | ref | configuration ID/generation name |
| 232 | 8 | ref | calibration ID |
| 240 | 8 | ref | device serial number |
| 248 | 8 | ref | comments |

Capture/device UUIDs and both SHA-256 digests are nonzero. Product, firmware,
build, sensor-profile, and configuration IDs are nonempty. Location and human
identity strings are capture-time values: an intentionally blank configured
value remains blank and is never filled from the device's current settings.

Topology is `0=unknown`, `1=wye`, or `2=delta`. Calibration status is
`0=unknown`, `1=valid`, `2=expired`, or `3=invalid`.

## Timebase-segments section

Each 128-byte record describes a contiguous range in the **persisted frame
domain** and the exact acquisition frames represented by it.

| Offset | Size | Type | Field |
|---:|---:|---|---|
| 0 | 8 | `u64` | first persisted frame index |
| 8 | 8 | `u64` | persisted frame count, nonzero |
| 16 | 8 | `u64` | first acquisition-frame sequence |
| 24 | 8 | `u64` | acquisition sequence step per persisted frame |
| 32 | 8 | `u64` | acquisition-rate numerator |
| 40 | 8 | `u64` | acquisition-rate denominator |
| 48 | 8 | `u64` | persisted-rate numerator |
| 56 | 8 | `u64` | persisted-rate denominator |
| 64 | 8 | `u64` | correlation acquisition sequence |
| 72 | 8 | `u64` | correlated PL tick |
| 80 | 8 | `u64` | correlated TAI nanoseconds |
| 88 | 8 | `u64` | correlated UTC nanoseconds |
| 96 | 8 | `u64` | correlation uncertainty, nanoseconds |
| 104 | 4 | `u32` | decimation divisor |
| 108 | 2 | `u16` | decimation method |
| 110 | 2 | `u16` | clock source |
| 112 | 2 | `u16` | time quality |
| 114 | 2 | `u16` | flags |
| 116 | 4 | `s32` | UTC-to-local offset active at capture, seconds |
| 120 | 8 | `u64` | exact acquisition frames represented |

Rates are positive exact rationals. Persisted rate equals acquisition rate
divided by the divisor after rational reduction.

Decimation method is `0=none` or `1=boxcar mean toward zero`. Method 0 requires
divisor, sequence step, and source frames all equal to the un-decimated
geometry. Method 1 requires `sequence_step == divisor`; every group except the
last has `divisor` acquisition frames, and the explicit source-frame count
preserves the size of a short final group.

Clock source is `0=unknown`, `1=system`, `2=PTP`, `3=GNSS`, or `4=manual`.
Time quality is `0=unknown`, `1=unlocked`, `2=holdover`, or `3=locked`.

Time flags are:

| Bit | Meaning |
|---:|---|
| 0 | UTC offset/context is known |
| 1 | positive leap second pending |
| 2 | negative leap second pending |
| 3 | rate or decimation changes before this segment |
| 4 | acquisition-sequence gap before this segment |

Positive and negative leap flags cannot coexist. The first segment cannot
declare a preceding gap or rate change. Later gap and rate-change flags must
agree exactly with the sequence and rational-rate geometry. Persisted frame
ranges are contiguous even when acquisition sequences have a declared gap.

For zero-based persisted frame `i` inside one segment:

```text
group_first_sequence = first_sequence + i * sequence_step
```

For boxcar data, the final group's acquisition-frame count is:

```text
source_frame_count - (frame_count - 1) * decimation_divisor
```

An acquisition sequence `q` can be correlated to TAI without external state:

```text
tai_ns(q) = correlation_tai_ns +
    (q - correlation_sequence) * 1_000_000_000 *
    acquisition_rate_denominator / acquisition_rate_numerator
```

The converter selects and documents its integer rounding rule. UTC, active
offset, leap context, clock source, quality, and uncertainty remain available
alongside the result.

## Channel-definitions section

One 208-byte record describes each interleaved sample word. Record order, not
`source_channel`, defines frame order. Stable UUID and source-channel values
are unique within a file.

| Offset | Size | Type | Field |
|---:|---:|---|---|
| 0 | 16 | bytes | stable channel UUID |
| 16 | 4 | `u32` | acquisition source-channel number |
| 20 | 4 | `u32` | validity flags |
| 24 | 2 | `u16` | phase |
| 26 | 2 | `u16` | quantity |
| 28 | 2 | `u16` | SI unit |
| 30 | 2 | `u16` | sample encoding |
| 32 | 2 | `u16` | stored bits |
| 34 | 2 | `u16` | valid signed bits |
| 36 | 2 | `s16` | preferred display decimal exponent |
| 38 | 2 | `u16` | reserved, zero |
| 40 | 8 | `s64` | affine gain numerator |
| 48 | 8 | `u64` | affine gain denominator |
| 56 | 8 | `s64` | affine offset numerator |
| 64 | 8 | `u64` | affine offset denominator |
| 72 | 8 | `u64` | primary/secondary ratio numerator |
| 80 | 8 | `u64` | primary/secondary ratio denominator |
| 88 | 8 | `s64` | nominal quantity numerator |
| 96 | 8 | `u64` | nominal quantity denominator |
| 104 | 8 | `s64` | characterized minimum numerator |
| 112 | 8 | `u64` | characterized minimum denominator |
| 120 | 8 | `s64` | characterized maximum numerator |
| 128 | 8 | `u64` | characterized maximum denominator |
| 136 | 8 | `u64` | resolution numerator |
| 144 | 8 | `u64` | resolution denominator |
| 152 | 8 | `s64` | raw clipping-low threshold |
| 160 | 8 | `s64` | raw clipping-high threshold |
| 168 | 8 | ref | channel name |
| 176 | 8 | ref | unit symbol |
| 184 | 8 | ref | description |
| 192 | 16 | bytes | reserved, zero |

Flags 0 through 7 respectively mean enabled, affine transform valid,
primary/secondary ratio valid, nominal valid, range valid, resolution valid,
clipping bounds valid, and calibration valid. Enabled is mandatory in v4
section version 1. A valid rational has a nonzero denominator.

The engineering value is in the declared SI unit:

```text
engineering = raw * gain_numerator / gain_denominator
            + offset_numerator / offset_denominator
```

The display exponent affects presentation only. The exact affine transform
and ratio, not a current sensor profile, are the export authority.

Phase is `0=none`, `1=A`, `2=B`, `3=C`, `4=neutral`, `5=AB`, `6=BC`, or
`7=CA`. Quantity is `0=unknown`, `1=current`, `2=voltage`, `3=status`,
`4=frequency`, or `5=ratio`. SI unit is `0=dimensionless`, `1=ampere`,
`2=volt`, or `3=hertz`.

Section version 1 permits only signed little-endian integer encoding (`1`).
Stored bits are a nonzero multiple of eight up to 64; valid signed bits are no
greater than stored bits. Channels in one frame are simultaneous by format
definition, so a COMTRADE channel skew derived from this section is zero.

## Event-descriptors section

There may be zero through 4096 event records. Each 256-byte record is the
latest capture-time descriptor for one stable event UUID; duplicate UUIDs are
invalid.

| Offset | Size | Type | Field |
|---:|---:|---|---|
| 0 | 16 | bytes | event UUID |
| 16 | 2 | `u16` | taxonomy |
| 18 | 2 | `u16` | nonzero event type within taxonomy |
| 20 | 2 | `u16` | lifecycle |
| 22 | 2 | `u16` | time quality |
| 24 | 4 | `u32` | validity/quality flags |
| 28 | 4 | `u32` | affected phase mask |
| 32 | 2 | `u16` | affected quantity |
| 34 | 2 | `u16` | engineering SI unit |
| 36 | 2 | `u16` | trigger source |
| 38 | 2 | `u16` | reserved, zero |
| 40 | 4 | `u32` | evaluated configuration generation |
| 44 | 4 | `u32` | severity |
| 48 | 8 | `u64` | start acquisition sequence |
| 56 | 8 | `u64` | current/update acquisition sequence |
| 64 | 8 | `u64` | end acquisition sequence |
| 72 | 8 | `u64` | trigger acquisition sequence |
| 80 | 32 | 4 * `u64` | start/current/end/trigger TAI nanoseconds |
| 112 | 32 | 4 * `u64` | start/current/end/trigger UTC nanoseconds |
| 144 | 8 | `u64` | time uncertainty, nanoseconds |
| 152 | 8 | `s64` | reference value, micro-SI unit |
| 160 | 8 | `s64` | evaluated threshold, micro-SI unit |
| 168 | 8 | `s64` | evaluated hysteresis, micro-SI unit |
| 176 | 24 | 3 * `s64` | phase A/B/C extrema, micro-SI unit |
| 200 | 8 | `u64` | duration in acquisition samples |
| 208 | 8 | `u64` | lifecycle update count |
| 216 | 4 | `u32` | producer status/detail bits |
| 220 | 4 | `u32` | reserved, zero |
| 224 | 8 | ref | stable taxonomy name |
| 232 | 8 | ref | event label |
| 240 | 8 | ref | exact evaluated settings snapshot, UTF-8 JSON |
| 248 | 8 | `u64` | reserved, zero |

Taxonomy is `0=unknown`, `1=IEC 61000-4-30`, or `2=MSAP1 product alarm`.
Lifecycle is `1=START`, `2=UPDATE`, `3=END`, `4=ABORT`, or `5=COMPLETE`.

Phase-mask bits 0 through 4 mean A, B, C, neutral, and system/aggregate. At
least one known bit is set. Validity flags are:

| Bit | Meaning |
|---:|---|
| 0 | start sequence is valid; mandatory |
| 1 | current/update sequence is valid |
| 2 | end sequence is valid |
| 3 | trigger sequence is valid |
| 4 | TAI anchors are valid |
| 5 | UTC anchors are valid |
| 6 | settings-snapshot JSON is present |
| 7 | event interval is contaminated |
| 8 | event interval is discontinuous |

Sequence anchors are in acquisition space, are ordered, and must lie in a
stored timebase segment rather than in a declared gap. Timestamp fields are
interpreted only when their validity bit is set. Settings-snapshot presence
and its flag must agree. Historical interpretation never queries current
settings.

A later converter may create a deterministic event-active status series
without a stored digital sample payload: for each descriptor it is active
from the start through the valid end sequence, inclusive. If no end is valid,
it remains active through the last source sequence represented by this master
and is labelled incomplete. The event UUID, lifecycle, contamination, and
discontinuity flags prevent that projection from masquerading as sampled
hardware state.

## Quality-intervals section

Each 64-byte record annotates stored frames and/or an acquisition-sequence
interval. This permits a zero-frame record to describe samples missing at a
declared gap.

| Offset | Size | Type | Field |
|---:|---:|---|---|
| 0 | 8 | `u64` | first persisted frame, or insertion point for a gap |
| 8 | 8 | `u64` | persisted frame count; zero is permitted |
| 16 | 8 | `u64` | first acquisition sequence |
| 24 | 8 | `u64` | last acquisition sequence, inclusive |
| 32 | 8 | `u64` | affected channel mask; zero means all channels |
| 40 | 4 | `u32` | quality flags, nonzero |
| 44 | 2 | `u16` | severity |
| 46 | 2 | `u16` | producer/source code |
| 48 | 4 | `u32` | producer detail code |
| 52 | 4 | `u32` | reserved, zero |
| 56 | 8 | `u64` | reserved, zero |

Flags 0 through 6 mean gap, saturated, clipped, transport loss, timing
uncertain, calibration invalid, and rate change. A nonzero channel bit must
name an existing channel. Frame ranges cannot exceed sample data; acquisition
sequence ranges cannot be reversed.

## Lineage section

Each 64-byte record links a master, continuation, virtual slice, or event.

| Offset | Size | Type | Field |
|---:|---:|---|---|
| 0 | 2 | `u16` | relation |
| 2 | 2 | `u16` | flags, currently zero |
| 4 | 4 | `u32` | reserved, zero |
| 8 | 16 | bytes | related capture UUID, nonzero |
| 24 | 16 | bytes | related event UUID, or zero when not applicable |
| 40 | 8 | `u64` | first related acquisition sequence |
| 48 | 8 | `u64` | last related acquisition sequence, inclusive |
| 56 | 4 | `u32` | zero-based part index |
| 60 | 4 | `u32` | nonzero part count |

Relations are `1=parent`, `2=previous continuation`, `3=next continuation`,
`4=event`, and `5=virtual slice`. Part index is less than part count. This
section lets one event reference contiguous capacity-limited masters and lets
an exported virtual slice retain its parent and continuation identity.

## Sample-data section

This section has no blob. `item_count` is the persisted frame count and
`item_bytes` is the sum of `stored_bits / 8` across channel definitions.
Records are interleaved in channel-definition order:

```text
frame 0: channel 0, channel 1, ... channel N-1
frame 1: channel 0, channel 1, ... channel N-1
...
```

There is no per-frame timestamp, sequence, or padding word. Timebase segments
cover every frame exactly once. Method `none` identifies raw signed acquisition
counts; method `boxcar mean toward zero` identifies signed averages in the same
count domain. A reader preserves the stored integer and uses channel metadata
for engineering conversion.

## Required defensive validation

Before returning typed metadata or sample spans, a reader rejects at least:

1. bad magic/version/header geometry or a declared file size that differs
   from the actual byte span;
2. a count or section above its defensive bound;
3. overflow, truncation, misalignment, nonzero padding, overlaps, holes, or
   unreferenced trailing bytes;
4. a header, directory, or section CRC32C mismatch;
5. a missing or duplicated mandatory section, an unknown required section,
   or an unsupported mandatory-section version;
6. disagreement between a directory entry and its known-section envelope;
7. invalid UTF-8, an escaping string reference, a zero required identity, or
   a nonzero reserved field/unknown flag;
8. a zero rational denominator, impossible sample/decimation geometry, an
   undeclared gap/rate change, or incomplete timebase coverage;
9. duplicate channel/event identities, invalid channel frame geometry, or a
   sample frame size that disagrees with the channels;
10. out-of-order or gap-resident event anchors, quality references outside
    the frame/channel domain, or invalid lineage part geometry.

CRC32C detects accidental corruption; it is not a digital signature. Import
and download authorization remains outside the file format.

## Forward compatibility

- A reader rejects an unsupported top-level version.
- Changing an existing record size or field meaning requires a new section
  version or top-level format version.
- New optional sections use a new type with directory required bit clear.
- A writer cannot use a new flag bit in a section-version-1 record unless old
  readers can safely reject or ignore it according to this contract.
- A virtual-slice writer recalculates offsets, counts, timebase coverage,
  directory entries, every CRC, and parent/event lineage. It never copies
  stale integrity values from the master.
