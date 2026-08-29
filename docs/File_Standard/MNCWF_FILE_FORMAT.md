# MNCWF waveform file format

## Status and ownership

This document is the normative reader specification for MSAP1 `.mncwf`
waveform capture files. It is intended for:

- product Web and CLI implementations;
- offline waveform viewers and export tools;
- test and calibration utilities;
- engineers diagnosing captures; and
- AI-assisted diagnostic tools.

The format is owned by the Linux acquisition implementation in `MSAP1_APU`.
The production writer emits version 4. Its authoritative implementation and
public data definitions are:

```text
common/msap1/waveform/waveform_capture.cpp
common/msap1/waveform/waveform_capture.hpp
```

Consumers in other repositories should link to this document instead of
maintaining a second copy of the binary definition.

The version 4 section-directory contract, encoder, defensive reader, and
capture-time metadata rules are specified in
[`MNCWF_V4_FILE_FORMAT.md`](MNCWF_V4_FILE_FORMAT.md). Versions 1 through 3
remain accepted for existing persisted history. Future COMTRADE and PQDIF
programs are converters from MNCWF v4, not alternate on-device recorders.
Their source-field matrix is in
[`MNCWF_V4_CONVERSION_READINESS.md`](MNCWF_V4_CONVERSION_READINESS.md).

Completed captures are stored under:

```text
/data/mnc/waveform/
```

The production writer uses this UTC filename convention:

```text
waveform-<session-id>-YYYY-MM-DD_HH-MM-SS-mmm.mncwf
```

For example:

```text
waveform-12-2026-07-30_20-52-08-006.mncwf
```

The filename is descriptive only. A reader must use the metadata inside the
file as the authoritative source. The daemon writes a `.tmp` file and renames
it to `.mncwf` only after the complete payload has been written successfully.
An incomplete capture or a capture intersecting a known transport gap is not
published as a valid `.mncwf` file.

## MNCWF is not the WFM1 DMA transport

Two related formats exist, but they serve different purposes:

| Format | Scope | Header | Channels | Purpose |
|---|---|---:|---:|---|
| `WFM1` | PL-to-Linux DMA transport | 64 bytes | 8 | Fixed 32,832-byte blocks used by the kernel driver and acquisition daemon |
| `.mncwf` | Persistent capture file | 64-byte header plus section directory in v4 | 7 | Variable-length, self-describing product waveform archive |

Each `WFM1` block contains 1024 frames of eight signed 32-bit channel words.
The daemon uses these blocks to maintain its rolling history. When a completed
trigger window is materialized, it creates an `.mncwf` file, adds timing,
event, and conversion metadata, and omits diagnostic CH7.

Do not parse an `.mncwf` file as a sequence of `WFM1` blocks. `WFM1` headers
are not copied into the persistent file.

## Binary conventions

Unless a field explicitly says otherwise:

- all multi-byte integers are little-endian;
- structures are packed and contain no implicit compiler padding;
- offsets are absolute byte offsets from the start of the file;
- sequence ranges are inclusive;
- `u32` and `u64` are unsigned 32-bit and 64-bit integers;
- `s32` is a two's-complement signed 32-bit integer;
- strings have fixed storage and are NUL-terminated when shorter than their
  field;
- reserved bytes are written as zero and must be ignored by readers; and
- every offset, count, size, and multiplication must be treated as untrusted
  input.

A reader must not cast unaligned file data directly to a native C or C++
structure. Decode little-endian fields explicitly or copy them into a packed
structure after validating the available size.

## Channel model

Versions 2 and 3 store seven product channels:

| Stored index | ADC source | Name | Kind | Engineering unit |
|---:|---:|---|---|---|
| 0 | CH0 | `Ia` | current | A |
| 1 | CH1 | `Ib` | current | A |
| 2 | CH2 | `Ic` | current | A |
| 3 | CH3 | `In` | current | A |
| 4 | CH4 | `Vc` | voltage | V |
| 5 | CH5 | `Vb` | voltage | V |
| 6 | CH6 | `Va` | voltage | V |

ADC CH7 remains present in the live PL/DMA transport for internal diagnostics,
but it is deliberately omitted from version 2 and 3 product capture files.
Version 1 files contain all eight legacy channels and remain readable.

Each stored sample is the signed 24-bit AD7771 value sign-extended to `s32`.
The upper eight bits must therefore equal the sign extension of bit 23 for
files produced by the current capture path. Readers should preserve all 32
bits and should not silently mask the value to 24 bits.

Version 2 and version 3 with `decimation == 1` retain each acquisition sample
exactly. Version 3 with a larger divisor stores boxcar means in ADC-count
units, as specified below; such a frame is not an individual raw conversion.

### Raw-to-engineering conversion

The file stores sample words only once. Each version 2 or 3 channel descriptor
contains the profile-specific conversion scale active when the capture was
created. This lets a viewer switch between raw ADC counts and converted
amperes or volts without duplicating the sample payload.

The descriptor scale is an **unsigned Q16.16 micro-unit per count**:

```text
scale_micro_units_per_count =
    scale_micro_units_q16 / 65536

value_micro_units =
    raw_count * scale_micro_units_q16 / 65536

value_engineering_units =
    raw_count * scale_micro_units_q16 /
    (65536 * 1,000,000)
```

For a current channel, the micro-unit is a microampere and the final unit is
amperes. For a voltage channel, the micro-unit is a microvolt and the final
unit is volts.

Use signed 64-bit or wider arithmetic for the multiplication:

```text
signed_product = int64(raw_count) * uint64(scale_micro_units_q16)
```

Do not apply the scale when descriptor flag bit 0 is clear. Such a channel is
raw-only for that capture. The conversion coefficients are a capture-time
snapshot; a later meter profile change must not alter conversion of an older
file.

## Versions 2 and 3 file layout

```text
+-----------------------------+ offset 0
| 256-byte fixed header       |
+-----------------------------+ channel_table_offset
| channel_count descriptors   | channel_descriptor_bytes each
+-----------------------------+ event_table_offset
| event_count trigger events  | 24 bytes each
+-----------------------------+ frame_data_offset
| frame_count waveform frames | frame_bytes each
+-----------------------------+ exact end of file
```

The version 2 writer and current version 3 writer emit all regions contiguously
without padding.

### Fixed header

The version 2 and 3 fixed header is exactly 256 bytes:

| Offset | Size | Type | Field |
|---:|---:|---|---|
| 0 | 8 | bytes | Magic: `MNCWF1\0\0` |
| 8 | 4 | `u32` | Format version: `2` or `3` |
| 12 | 4 | `u32` | Header bytes: `256` |
| 16 | 8 | `u64` | Session ID |
| 24 | 8 | `u64` | First frame sequence |
| 32 | 8 | `u64` | Last frame sequence, inclusive |
| 40 | 8 | `u64` | Primary trigger frame sequence |
| 48 | 8 | `u64` | Primary trigger `CLOCK_TAI`, nanoseconds |
| 56 | 4 | `u32` | Measured sample rate, frames/s |
| 60 | 4 | `u32` | Trigger event count |
| 64 | 8 | `u64` | Correlation `CLOCK_TAI`, nanoseconds |
| 72 | 8 | `u64` | Correlated PL tick |
| 80 | 8 | `u64` | Correlated frame sequence |
| 88 | 8 | `u64` | Correlation uncertainty, nanoseconds |
| 96 | 4 | `u32` | Stored channel count; currently `7` |
| 100 | 4 | `u32` | Bytes per frame; currently `28` |
| 104 | 4 | `u32` | Bytes per channel descriptor; currently `32` |
| 108 | 4 | `u32` | File flags; currently zero |
| 112 | 8 | `u64` | Channel-table offset; currently `256` |
| 120 | 8 | `u64` | Event-table offset |
| 128 | 8 | `u64` | Frame-data offset |
| 136 | 8 | `u64` | Frame count |
| 144 | 8 | `u64` | Primary trigger `CLOCK_REALTIME`, nanoseconds |
| 152 | 4 | `u32` | Version 3 decimation divisor; this location is zero/reserved in version 2 |
| 156 | 100 | bytes | Reserved; currently zero |

#### Version 3 decimation

Version 3 adds only the `decimation` field at offset 152; every other header,
descriptor, event, and frame field keeps its version 2 layout and meaning.
The current writer accepts divisors `1`, `2`, `4`, `8`, `16`, and `32`.
Readers must use an implicit divisor of `1` for version 1 and version 2 files.

For a divisor greater than one, each stored channel word is the signed integer
mean, truncated toward zero, of a consecutive group of up to `decimation`
acquisition frames. The last group may contain fewer frames. The header keeps
`sample_rate_hz` and all sequences in the acquisition-frame domain; therefore:

```text
effective_sample_rate_hz = sample_rate_hz / decimation
sequence(n) = first_sequence + n * decimation
frame_count = (last_sequence - first_sequence) / decimation + 1
```

`last_sequence` identifies the first acquisition frame folded into the last
stored frame. Version 3 does not retain the number of source frames in a short
final group. A converter that requires that distinction must not guess it;
MNCWF v4 will carry an explicit rate/decimation segment contract.

#### Session and trigger fields

`session_id` is assigned by the acquisition daemon and identifies the capture
within the device's persistent waveform history. It is not globally unique.

`trigger_sequence` and the two trigger timestamps describe the primary event
that created the capture. If overlapping triggers are merged, the event table
contains every trigger. Readers should use the event table to display all
markers rather than assuming there is only one event.

`trigger_realtime_nanoseconds` is intended for filenames and human calendar
display. `trigger_tai_nanoseconds` is the stable time base for event
correlation. `CLOCK_REALTIME` may be adjusted by time synchronization or an
administrator.

#### PL/time correlation fields

The correlation tuple relates one PL tick and waveform frame sequence to
`CLOCK_TAI`:

```text
(correlation_pl_tick, correlation_frame_sequence)
    <-> correlation_tai_nanoseconds
```

`correlation_uncertainty_nanoseconds` bounds the observation interval used to
obtain that relationship. It is diagnostic metadata for correlation with
other PL or system events. Normal waveform plotting can use frame sequence and
the measured sample rate.

### Channel descriptor

Each version 2 and 3 channel descriptor is exactly 32 bytes:

| Offset | Size | Type | Field |
|---:|---:|---|---|
| 0 | 4 | `u32` | Original ADC source channel |
| 4 | 4 | `u32` | Kind: `1=current`, `2=voltage`, `3=debug` |
| 8 | 4 | `u32` | Unsigned Q16.16 micro-unit/count scale |
| 12 | 4 | `u32` | Flags; bit 0 means conversion is valid |
| 16 | 8 | char | NUL-terminated channel name |
| 24 | 8 | char | NUL-terminated engineering unit |

The descriptor order defines the word order in every sample frame. Do not
infer frame ordering from `source_channel`, and do not assume future files
always contain exactly seven channels.

Unknown kind values or flag bits should be preserved and reported as unknown,
not reinterpreted as a known channel type.

### Trigger event

Each event entry is exactly 24 bytes:

| Offset | Size | Type | Field |
|---:|---:|---|---|
| 0 | 8 | `u64` | Event frame sequence |
| 8 | 8 | `u64` | Event `CLOCK_TAI`, nanoseconds |
| 16 | 4 | `u32` | Source: `1=CLI`, `2=Web`, `3=PQ event` |
| 20 | 4 | `u32` | Reserved; currently zero |

An event sequence should fall within the stored inclusive frame range.
Overlapping manual and future PQ-event windows may share one capture, so
`event_count` may be greater than one. Unknown source values should still be
shown as event markers with an unknown source.

### Sample frames

Each frame is an interleaved array of `channel_count` signed 32-bit raw ADC
counts:

```text
frame 0: sample[0], sample[1], ... sample[channel_count - 1]
frame 1: sample[0], sample[1], ... sample[channel_count - 1]
...
```

There is no timestamp, sequence, or padding word inside an individual frame.
Let `decimation` be the version 3 field, or `1` for a version 1 or 2 file. For
zero-based stored-frame index `n`:

```text
sequence(n) = first_sequence + n * decimation
file_offset(n) = frame_data_offset + n * frame_bytes
```

The primary trigger need not fall exactly on a stored decimation-group anchor.
Its fractional position in the stored-frame domain is:

```text
trigger_index = (trigger_sequence - first_sequence) / decimation
```

Time relative to the primary trigger is:

```text
relative_seconds(n) =
    (sequence(n) - trigger_sequence) / sample_rate_hz
```

An approximate absolute TAI timestamp can be calculated as:

```text
tai_nanoseconds(n) =
    trigger_tai_nanoseconds +
    round((sequence(n) - trigger_sequence) * 1,000,000,000 /
          sample_rate_hz)
```

The same equation can use `trigger_realtime_nanoseconds` for human display,
but realtime is not the preferred clock for cross-system event correlation.

## Versions 2 and 3 validation rules

A complete version 2 or 3 file satisfies all of these invariants:

```text
magic == "MNCWF1\0\0"
version in {2, 3}
header_bytes == 256
channel_count > 0
channel_count <= 8
channel_descriptor_bytes == 32
frame_bytes == channel_count * 4
channel_table_offset == 256
event_table_offset ==
    channel_table_offset + channel_count * channel_descriptor_bytes
frame_data_offset ==
    event_table_offset + event_count * 24
last_sequence >= first_sequence
(last_sequence - first_sequence) % decimation == 0
frame_count == (last_sequence - first_sequence) / decimation + 1
file_size == frame_data_offset + frame_count * frame_bytes
```

For version 2, `decimation` in these equations is the implicit value `1` and
the four bytes at offset 152 are reserved. For version 3, `decimation` must be
one of `1`, `2`, `4`, `8`, `16`, or `32`.

Also validate:

- every addition and multiplication for a range calculation cannot overflow;
- every table and payload range lies within the actual file;
- declared regions are ordered and do not overlap;
- channel names and units are decoded from at most eight bytes;
- each event sequence lies within the frame range; and
- `trigger_sequence` lies within the frame range.

The current daemon requires the exact expected file size when rediscovering
captures after a reboot. Appending private data to the file is therefore not
supported.

## Version 2 and version 1 compatibility

Version 2 has the same descriptors and region layout as version 3, but it has
no decimation field. Readers must interpret version 2 offset 152 as reserved
and use `decimation = 1`; they must not reject a valid version 2 file because
that reserved value is zero.

Legacy version 1 has a 128-byte header. Fields at offsets 0 through 95 match
the corresponding fields in version 2. Version 1 does not contain the version
2 channel-count, descriptor, or offset fields.

Its layout is:

```text
+----------------------------+ offset 0
| 128-byte legacy header     |
+----------------------------+ offset 128
| event_count events         | 24 bytes each
+----------------------------+
| eight-channel frames       | 8 * s32 = 32 bytes each
+----------------------------+ exact end of file
```

For version 1:

```text
frame_count = last_sequence - first_sequence + 1
frame_data_offset = 128 + event_count * 24
file_size = frame_data_offset + frame_count * 32
```

Version 1 has no conversion metadata. A reader should label its samples CH0
through CH7 and present raw counts only. It must not apply the currently
installed meter profile to a legacy capture because that profile may differ
from the one active when the file was recorded.

## Defensive Python reader

This example demonstrates the binary contract and reads frames on demand. A
production viewer should use `mmap` for large captures instead of loading the
whole file.

```python
from dataclasses import dataclass
from pathlib import Path
import struct

MAGIC = b"MNCWF1\0\0"
EVENT_BYTES = 24


def checked_range(file_size: int, offset: int, count: int, item_size: int):
    if offset < 0 or count < 0 or item_size < 0:
        raise ValueError("negative file range")
    end = offset + count * item_size
    if end < offset or end > file_size:
        raise ValueError("file range is truncated or overflows")
    return offset, end


def fixed_string(value: bytes) -> str:
    return value.split(b"\0", 1)[0].decode("utf-8", errors="replace")


@dataclass
class Channel:
    source: int
    kind: int
    scale_q16: int
    valid: bool
    name: str
    unit: str

    def convert(self, raw: int):
        if not self.valid:
            return None
        return raw * self.scale_q16 / (65536.0 * 1_000_000.0)


class Mncwf:
    def __init__(self, path: str):
        self.path = Path(path)
        self.data = memoryview(self.path.read_bytes())
        if len(self.data) < 16:
            raise ValueError("truncated MNCWF header")

        magic, self.version, self.header_bytes = struct.unpack_from(
            "<8sII", self.data, 0)
        if magic != MAGIC:
            raise ValueError("not an MNCWF file")
        if self.version not in (1, 2, 3):
            raise ValueError(f"unsupported MNCWF version {self.version}")

        if len(self.data) < self.header_bytes:
            raise ValueError("truncated MNCWF header")

        (
            self.session_id,
            self.first_sequence,
            self.last_sequence,
            self.trigger_sequence,
            self.trigger_tai_ns,
        ) = struct.unpack_from("<QQQQQ", self.data, 16)
        self.sample_rate_hz, self.event_count = struct.unpack_from(
            "<II", self.data, 56)

        if self.last_sequence < self.first_sequence:
            raise ValueError("invalid sequence range")
        self.decimation = 1
        sequence_span = self.last_sequence - self.first_sequence
        sequence_frame_count = sequence_span + 1

        if self.version >= 2:
            if self.header_bytes != 256:
                raise ValueError("invalid version 2/3 header size")
            (
                self.channel_count,
                self.frame_bytes,
                descriptor_bytes,
                self.flags,
            ) = struct.unpack_from("<IIII", self.data, 96)
            (
                channel_offset,
                self.event_offset,
                self.frame_offset,
                self.frame_count,
                self.trigger_realtime_ns,
            ) = struct.unpack_from("<QQQQQ", self.data, 112)

            if self.version >= 3:
                self.decimation = struct.unpack_from(
                    "<I", self.data, 152)[0]
                if self.decimation not in (1, 2, 4, 8, 16, 32):
                    raise ValueError("invalid version 3 decimation")

            if not 0 < self.channel_count <= 8:
                raise ValueError("invalid channel count")
            if descriptor_bytes != 32:
                raise ValueError("unsupported channel descriptor size")
            if self.frame_bytes != self.channel_count * 4:
                raise ValueError("invalid frame size")
            if channel_offset != self.header_bytes:
                raise ValueError("invalid channel table offset")
            if self.event_offset != channel_offset + self.channel_count * 32:
                raise ValueError("invalid event table offset")
            if self.frame_offset != self.event_offset + self.event_count * 24:
                raise ValueError("invalid frame data offset")

            checked_range(len(self.data), channel_offset,
                          self.channel_count, 32)
            self.channels = []
            for index in range(self.channel_count):
                offset = channel_offset + index * descriptor_bytes
                source, kind, scale, flags, name, unit = struct.unpack_from(
                    "<IIII8s8s", self.data, offset)
                self.channels.append(Channel(
                    source, kind, scale, bool(flags & 1),
                    fixed_string(name), fixed_string(unit)))
        else:
            if self.header_bytes != 128:
                raise ValueError("invalid version 1 header size")
            self.channel_count = 8
            self.frame_bytes = 32
            self.frame_count = sequence_frame_count
            self.event_offset = 128
            self.frame_offset = (
                self.event_offset + self.event_count * EVENT_BYTES
            )
            self.trigger_realtime_ns = None
            self.flags = 0
            self.channels = [
                Channel(i, 3, 0, False, f"CH{i}", "")
                for i in range(8)
            ]

        if sequence_span % self.decimation:
            raise ValueError("sequence range is not decimation-aligned")
        sequence_frame_count = sequence_span // self.decimation + 1
        if self.frame_count != sequence_frame_count:
            raise ValueError("frame count and sequence range disagree")
        self.effective_sample_rate_hz = (
            self.sample_rate_hz / self.decimation
        )

        checked_range(len(self.data), self.event_offset,
                      self.event_count, EVENT_BYTES)
        _, expected_size = checked_range(
            len(self.data), self.frame_offset,
            self.frame_count, self.frame_bytes)
        if expected_size != len(self.data):
            raise ValueError("unexpected trailing MNCWF data")
        if not (self.first_sequence <= self.trigger_sequence
                <= self.last_sequence):
            raise ValueError("trigger lies outside frame range")

    def event(self, index: int):
        if not 0 <= index < self.event_count:
            raise IndexError(index)
        offset = self.event_offset + index * EVENT_BYTES
        sequence, tai_ns, source, reserved = struct.unpack_from(
            "<QQII", self.data, offset)
        if not self.first_sequence <= sequence <= self.last_sequence:
            raise ValueError("event lies outside frame range")
        return {
            "sequence": sequence,
            "tai_ns": tai_ns,
            "source": source,
            "reserved": reserved,
        }

    def frame(self, index: int):
        if not 0 <= index < self.frame_count:
            raise IndexError(index)
        offset = self.frame_offset + index * self.frame_bytes
        return struct.unpack_from(
            f"<{self.channel_count}i", self.data, offset)

    def converted_frame(self, index: int):
        return [
            channel.convert(raw)
            for channel, raw in zip(self.channels, self.frame(index))
        ]

    def relative_seconds(self, index: int):
        sequence = self.first_sequence + index * self.decimation
        return (
            sequence - self.trigger_sequence
        ) / self.sample_rate_hz


capture = Mncwf("capture.mncwf")
print(capture.channels)
print(capture.frame(0))
print(capture.converted_frame(0))
```

Python's `int` has arbitrary precision, so the conversion multiplication does
not overflow. C++, Rust, JavaScript, and TypeScript readers must select types
carefully. In particular, JavaScript/TypeScript must decode 64-bit header
fields with `BigInt`; many frame sequence values are not safely represented by
a JavaScript `number`.

## Large-capture viewer guidance

A capture can contain millions of frames. A viewer should:

1. memory-map or range-read the file;
2. retain 64-bit sequences and offsets without converting them to floating
   point;
3. read only the visible frame window;
4. aggregate minimum/maximum envelopes per horizontal display pixel;
5. convert only the visible channels and visible window;
6. keep event markers indexed by sequence;
7. use the declared channel descriptors for labels and units; and
8. clamp zoom and cursor calculations to the validated frame range.

Do not create one language object, DOM node, or chart point for every stored
sample. Downsampling by simply selecting every Nth sample can hide narrow
transients; a min/max envelope is preferred.

## Integrity and security

Versions 1 through 3 have no embedded checksum, signature, compression, or
encryption. Version 4 adds CRC32C integrity for the header, directory, and
every section, as specified in the version 4 document; CRC is integrity error
detection, not authenticity or encryption. Files stored by the product are
protected by filesystem and authenticated download policy, but an external
`.mncwf` file must still be treated as untrusted binary input.

Structural validation can identify truncation and many corrupt layouts, but it
cannot prove the sample payload is authentic. If transport or archival
integrity requirements later require a checksum or signature, introduce it
through a documented format-version change rather than placing private bytes
after the current payload.

## Versioning rules for future writers

- A reader must reject an unsupported format version.
- Reserved fields and unknown flag bits must be ignored unless a future
  specification defines them as mandatory.
- Changing the meaning or byte layout of an existing field requires a new
  version.
- Adding optional behavior through currently reserved flags is permitted only
  when old readers can continue interpreting the core data safely.
- New writers should continue publishing atomically and should never expose a
  partially written `.mncwf` filename.
- A format change must update this document, the APU writer/reader tests, and
  all supported Web/offline readers in the same coordinated change.

## Reader safety checklist

1. Reject unknown magic, unsupported versions, and incorrect header sizes.
2. Treat every offset, count, size, and multiplication as untrusted.
3. Confirm table and sample ranges are ordered, non-overlapping, and within
   the exact file size.
4. Use little-endian signed interpretation for sample words.
5. Use 64-bit integer or `BigInt` handling for sequences, timestamps, counts,
   and offsets.
6. Do not infer channel ordering when version 2 or 3 descriptors are available.
7. Treat conversion-invalid descriptors as raw-only.
8. Use the capture's stored scale rather than the device's current profile.
9. Display every event in the event table, including unknown event sources.
10. Treat TAI as event-correlation time and realtime as filename/UI time.
11. For version 3, validate and apply the stored decimation divisor to every
    sequence, time-axis, and effective-rate calculation.
