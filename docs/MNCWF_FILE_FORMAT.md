# MNCWF waveform file format

This is the normative reader guide for MSAP1 `.mncwf` capture files. It is
written for engineers and AI-assisted diagnostic tools. All multi-byte
integers are little-endian.

## Channel model

Version 2 files preserve exact signed ADC counts for seven product channels:

| Stored index | ADC source | Name | Kind | Unit |
|---:|---:|---|---|---|
| 0 | CH0 | Ia | current | A |
| 1 | CH1 | Ib | current | A |
| 2 | CH2 | Ic | current | A |
| 3 | CH3 | In | current | A |
| 4 | CH4 | Vc | voltage | V |
| 5 | CH5 | Vb | voltage | V |
| 6 | CH6 | Va | voltage | V |

ADC CH7 remains present in the live PL/DMA transport for internal diagnostics,
but it is omitted from persisted version 2 files. Version 1 files contain all
eight legacy channels and remain readable.

The file stores raw values once. Every version 2 channel descriptor carries
the configuration active at capture time, so readers can switch between raw
counts and converted amperes/volts without duplicating the waveform:

```text
micro_units_q16 = raw_count * scale_micro_units_q16
engineering_units = micro_units_q16 / (65536 * 1,000,000)
```

Use signed 64-bit or wider arithmetic for the multiplication.

## Version 2 layout

```text
+-----------------------------+ offset 0
| 256-byte fixed header       |
+-----------------------------+ channel_table_offset
| channel_count descriptors   | 32 bytes each
+-----------------------------+ event_table_offset
| event_count trigger events  | 24 bytes each
+-----------------------------+ frame_data_offset
| frame_count waveform frames | frame_bytes each
+-----------------------------+
```

### Fixed header (256 bytes)

| Offset | Size | Type | Field |
|---:|---:|---|---|
| 0 | 8 | bytes | Magic: `MNCWF1\0\0` |
| 8 | 4 | `u32` | Format version: `2` |
| 12 | 4 | `u32` | Header bytes: `256` |
| 16 | 8 | `u64` | Session ID |
| 24 | 8 | `u64` | First frame sequence |
| 32 | 8 | `u64` | Last frame sequence, inclusive |
| 40 | 8 | `u64` | Trigger frame sequence |
| 48 | 8 | `u64` | Trigger `CLOCK_TAI`, nanoseconds |
| 56 | 4 | `u32` | Measured sample rate, frames/s |
| 60 | 4 | `u32` | Event count |
| 64 | 8 | `u64` | Correlation `CLOCK_TAI`, nanoseconds |
| 72 | 8 | `u64` | Correlated PL tick |
| 80 | 8 | `u64` | Correlated frame sequence |
| 88 | 8 | `u64` | Correlation uncertainty, nanoseconds |
| 96 | 4 | `u32` | Stored channel count (`7`) |
| 100 | 4 | `u32` | Frame bytes (`28`) |
| 104 | 4 | `u32` | Channel descriptor bytes (`32`) |
| 108 | 4 | `u32` | File flags; currently zero |
| 112 | 8 | `u64` | Channel-table offset |
| 120 | 8 | `u64` | Event-table offset |
| 128 | 8 | `u64` | Frame-data offset |
| 136 | 8 | `u64` | Frame count |
| 144 | 8 | `u64` | Trigger `CLOCK_REALTIME`, nanoseconds |
| 152 | 104 | bytes | Reserved |

Validate all declared ranges against the actual file size. A complete capture
satisfies:

```text
frame_count = last_sequence - first_sequence + 1
file_size >= frame_data_offset + frame_count * frame_bytes
```

### Channel descriptor (32 bytes)

| Offset | Size | Type | Field |
|---:|---:|---|---|
| 0 | 4 | `u32` | Original ADC source channel |
| 4 | 4 | `u32` | Kind: `1=current`, `2=voltage`, `3=debug` |
| 8 | 4 | `u32` | Unsigned Q16.16 micro-unit/count scale |
| 12 | 4 | `u32` | Flags; bit 0 means conversion is valid |
| 16 | 8 | char | NUL-terminated channel name |
| 24 | 8 | char | NUL-terminated engineering unit |

### Trigger event (24 bytes)

| Offset | Size | Type | Field |
|---:|---:|---|---|
| 0 | 8 | `u64` | Event frame sequence |
| 8 | 8 | `u64` | Event `CLOCK_TAI`, nanoseconds |
| 16 | 4 | `u32` | Source: `1=CLI`, `2=Web`, `3=PQ event` |
| 20 | 4 | `u32` | Reserved |

### Sample frames

Each frame is an interleaved array of `channel_count` signed 32-bit raw ADC
counts. The first frame corresponds to `first_sequence`; frame `n` has sequence
`first_sequence + n`.

## Version 1 compatibility

Legacy version 1 has a 128-byte header. Fields at offsets 0 through 95 match
version 2. Events begin at offset 128 and remain 24 bytes each. Sample data
follows and contains eight signed 32-bit channels per frame. Version 1 has no
channel descriptors or conversion scale, so a reader should label channels
CH0 through CH7 and present raw counts only.

## Minimal Python reader

```python
from pathlib import Path
import struct

data = memoryview(Path("capture.mncwf").read_bytes())
magic, version, header_bytes = struct.unpack_from("<8sII", data, 0)
if magic != b"MNCWF1\0\0":
    raise ValueError("not an MNCWF file")

session_id, first_seq, last_seq = struct.unpack_from("<QQQ", data, 16)
sample_rate_hz, event_count = struct.unpack_from("<II", data, 56)

if version == 2:
    channel_count, frame_bytes, descriptor_bytes = struct.unpack_from(
        "<III", data, 96)
    channel_offset, event_offset, frame_offset, frame_count = struct.unpack_from(
        "<QQQQ", data, 112)
    channels = []
    for index in range(channel_count):
        offset = channel_offset + index * descriptor_bytes
        source, kind, scale_q16, flags, name, unit = struct.unpack_from(
            "<IIII8s8s", data, offset)
        channels.append({
            "source": source,
            "kind": kind,
            "scale_q16": scale_q16,
            "valid": bool(flags & 1),
            "name": name.split(b"\0", 1)[0].decode(),
            "unit": unit.split(b"\0", 1)[0].decode(),
        })
elif version == 1:
    channel_count, frame_bytes, descriptor_bytes = 8, 32, 0
    event_offset = 128
    frame_offset = event_offset + event_count * 24
    frame_count = last_seq - first_seq + 1
    channels = [{"name": f"CH{i}", "source": i} for i in range(8)]
else:
    raise ValueError(f"unsupported MNCWF version {version}")

required = frame_offset + frame_count * frame_bytes
if required > len(data):
    raise ValueError("truncated MNCWF sample payload")

raw_frame_0 = struct.unpack_from(f"<{channel_count}i", data, frame_offset)
if version == 2:
    converted = [
        raw * channel["scale_q16"] / (65536.0 * 1_000_000.0)
        if channel["valid"] else None
        for raw, channel in zip(raw_frame_0, channels)
    ]
```

For large captures, do not unpack every sample into Python objects. Keep the
file memory-mapped and read only the requested frame window, or aggregate
min/max envelopes per display pixel.

## Reader safety checklist

1. Reject unknown magic, version, or undersized headers.
2. Treat every offset, count, and multiplication as untrusted input.
3. Confirm table and sample ranges fit within the file.
4. Use little-endian signed interpretation for sample words.
5. Do not infer ordering when version 2 descriptors are available.
6. Treat descriptors with conversion-valid clear as raw-only.
7. Treat TAI as event correlation time and realtime as filename/UI time.
