# MNCWF version 5 compressed sample format

## Status and compatibility

This is the normative byte contract for an `.mncwf` file whose magic is
`MNCWF1\0\0` and whose top-level version is `5`. Version 5 retains the version
4 header, directory, metadata sections, filename convention, channel model,
timebase, event descriptors, quality intervals, and lineage. It changes only
the sample-data section from version 1 to version 2.

The production acquisition writer emits version 5. Product readers accept
versions 1 through 5; existing version 4 files are not rewritten. Readers that
support only version 4 must reject version 5 by its top-level version rather
than attempting to interpret the compressed sample section.

The implementation is in
[`mncwf_v4.hpp`](../../common/msap1/waveform/mncwf_v4.hpp) and
[`mncwf_v4.cpp`](../../common/msap1/waveform/mncwf_v4.cpp). Cross-language
golden and corruption vectors are in
[`mncwf_v4_reader_test.cpp`](../../tests/mncwf_v4_reader_test.cpp) and the
MSAP1 Web `waveformFile.test.ts` suite.

## Top-level and metadata contract

The 64-byte header and every 56-byte directory entry use the version 4 layout
defined by [`MNCWF_V4_FILE_FORMAT.md`](MNCWF_V4_FILE_FORMAT.md), with these
version 5 requirements:

- header `version` is exactly `5`;
- mandatory section types 1 through 6 use section version 1 and are encoded
  exactly as in version 4;
- mandatory sample-data section type 7 uses section version 2;
- the sample directory entry's `stored_bytes` is the compressed section size;
- its `logical_bytes` is exactly `item_count * item_bytes`; and
- all other section directory entries keep `logical_bytes == stored_bytes`.

Metadata remains outside the compressed sample chunks. Archive discovery can
therefore validate header, directory, metadata CRCs, metadata values, sample
geometry, chunk descriptors, and chunk extents without decompressing sample
data or reading every sample payload page. A consumer must perform complete
sample-section and logical-chunk validation before exposing samples. Retention
also performs complete validation before deleting a candidate.

## Sample-data section version 2

The section starts with this 48-byte envelope. Offsets in the chunk table are
relative to the start of this sample section.

| Offset | Size | Type | Field |
|---:|---:|---|---|
| 0 | 4 | `u32` | section type, exactly `7` |
| 4 | 2 | `u16` | section version, exactly `2` |
| 6 | 2 | `u16` | envelope bytes, exactly `48` |
| 8 | 4 | `u32` | envelope flags, zero |
| 12 | 4 | `u32` | logical bytes per interleaved frame |
| 16 | 8 | `u64` | total logical frame count |
| 24 | 8 | `u64` | chunk-table offset, exactly `48` |
| 32 | 8 | `u64` | chunk-table bytes, a nonzero multiple of `56` |
| 40 | 8 | `u64` | reserved, zero |

The table contains one 56-byte entry per independent chunk:

| Offset | Size | Type | Field |
|---:|---:|---|---|
| 0 | 8 | `u64` | first logical frame |
| 8 | 8 | `u64` | logical frame count, nonzero |
| 16 | 8 | `u64` | aligned stored-data offset within this section |
| 24 | 8 | `u64` | stored bytes, nonzero |
| 32 | 8 | `u64` | logical bytes, exactly `frame_count * item_bytes` |
| 40 | 2 | `u16` | codec: `0=raw`, `1=Zstd` |
| 42 | 2 | `u16` | codec flags; bit 0 is the Zstd frame-checksum contract |
| 44 | 4 | `u32` | CRC32C of the logical, decompressed bytes |
| 48 | 8 | `u64` | reserved, zero |

The first stored payload begins at the eight-byte alignment of the table end.
Every later payload begins at the eight-byte alignment of the preceding
payload end. Alignment bytes are zero. The final aligned payload end equals
the sample section's `stored_bytes`; overlaps, holes, reordered chunks, and
trailing bytes are invalid.

Chunks cover frames consecutively from frame zero through `item_count - 1`
without overlap or omission. A chunk boundary is always a frame boundary and
one chunk contains at most 1 MiB of logical data. A file contains at most 4096
chunks, bounding the directory allocation even for hostile input. The complete
logical sample section and complete stored file are each bounded to 512 MiB by
the product reader.

## Codec policy

The production writer compresses each chunk independently with Zstd level 1,
no dictionary, a declared frame content size, a frame checksum, and a maximum
1 MiB window. If the resulting frame is not smaller than the logical bytes,
the writer stores that chunk raw.

For a raw chunk, flags are zero and `stored_bytes == logical_bytes`. For a
Zstd chunk, codec is 1, flags are exactly bit 0, `stored_bytes <
logical_bytes`, and the payload contains exactly one standard Zstd frame. Its
header must declare no dictionary, no reserved option, the checksum flag, the
exact chunk logical size, and a window no larger than 1 MiB. Concatenated or
trailing frames are invalid.

Zstd's frame checksum protects the compressed codec stream. The sample
section's directory CRC32C protects the complete stored chunk table and
payload bytes. Each chunk's logical CRC32C independently protects the exact
decompressed sample bytes. These checks detect corruption; they are not an
authenticity signature or encryption.

## Writer and export behavior

Waveform DMA/control code snapshots a bounded raw window only. A background
thread at the Background service tier performs decimation, chunk compression,
CRC generation, temporary-file output, complete decode validation, retention,
and atomic rename. Compression never runs in an HTTP request handler.

An event-specific version 5 export regenerates metadata and partial boundary
chunks. Any complete middle chunks are copied with their stored codec frame
and logical CRC unchanged. The resulting virtual file receives fresh offsets,
section/directory/header CRCs, capture UUID, timebase, and lineage. It is still
served as `application/x-mncwf`, without `Content-Encoding`.

## Required reader validation

In addition to every applicable version 4 check, a complete version 5 reader
rejects at least:

1. an unsupported sample-section version or envelope disagreement;
2. a logical allocation over 512 MiB, more than 4096 chunks, or a chunk over
   1 MiB;
3. a chunk-table overflow, truncation, overlap, hole, nonzero padding, or
   incomplete frame coverage;
4. an unknown codec or invalid raw/Zstd size and flag relationship;
5. a missing Zstd content size or checksum, dictionary use, reserved frame
   option, excessive window, malformed block, or more than one frame;
6. a stored sample-section CRC32C mismatch;
7. a decompressed-size or logical chunk CRC32C mismatch; and
8. sample/timebase/channel geometry disagreement after decompression.

The browser performs parsing, decompression, CRC checks, and min/max pyramid
construction in a module Web Worker using the pinned decompression-only WASM
decoder. Typed-array buffers are transferred to React only after validation.
