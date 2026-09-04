# M18 metrology co-release contract

Status: implementation contract, 2026-08-29. This document fixes the common
host/RPMsg/private-packet/record vocabulary for M18. It is not a declaration
of IEC 61000-4-30 Class A compliance or IEC 62586 product certification.

## Normative references and measurement baseline

Algorithm implementations and independent verification vectors are pinned to:

- IEC 61000-4-30:2025, *Power quality measurement methods*;
- IEC 61000-4-15:2010, *Flickermeter — Functional and design specifications*;
- IEC 62586-2:2017+A1:2021, *Functional tests and uncertainty requirements*.

The accepted pre-M18 implementation comparison point is the latest retained
K26 route: 50,092 LUT, 71,207 FF, 109.5 BRAM, 6 URAM, 247 DSP, and WNS
+0.322 ns. Each engine integration must report a delta from that point and the
release route must meet timing with no DRC errors or critical warnings while
retaining at least ten percent device headroom. A target acceptance result is
still required before any compliance statement can be evaluated.

M17 host work may continue concurrently, but its quadrant, 50 Hz, fixed-demand,
restart/reset, zero-loss, and measured R5 stack-headroom gates precede the first
M18 target deployment. Any M17 ABI or arithmetic fix must be merged and its
reference/host/Web gates rerun before the M18 interfaces are declared frozen.

## Version set

The M18 co-release moves these authorities together:

| Authority | M18 version | Compatibility rule |
| --- | ---: | --- |
| Product settings | schema 4 | Decode schema 1–3 and migrate in memory; preserve the existing voltage sag, swell, and interruption settings. |
| APU/RPU control | RPMsg 9 | Headers are byte-identical; configuration and acknowledgement only. |
| Acquisition IPC | 37 | Reject a peer with a different fixed protocol version. |
| ADC simulator | 1.5 | Adds deterministic AM and absolute carrier/adjacent tones. |
| Persistent waveform | MNCWF 4 | Sole M18 master format; current post-conversion exports also provide COMTRADE and PQDIF without changing capture. |

`msap1_meter_config_payload` remains a fixed 352-byte request. The independent
316-byte `msap1_m18_config_payload` keeps the complete RPMsg frame below the
384-byte platform bound. The APU coordinates one nonzero generation across
both messages; R5C0 validates and applies PL-facing state atomically, and R5C1
applies lifecycle/aggregation policy only at a record boundary.

## Event taxonomy and capability policy

The fixed profile order is voltage sag, voltage swell, voltage interruption,
rapid voltage change, voltage unbalance, current sag, current swell, current
unbalance, and transient voltage. Only the first four are IEC voltage-event
classifications. Voltage/current unbalance and current sag/swell are explicitly
MSAP1 product alarms.

Every profile independently snapshots enable, threshold, hysteresis, phase
policy, phase mask, waveform pre/post windows, and decimation. New current,
unbalance, and transient profiles migrate disabled. Transient voltage must be
rejected while enabled until the approximately 12.5 kHz analogue frontend and
the selected acquisition profiles have been characterized; the capability bit
therefore remains clear.

Stable event identities span `START`, `UPDATE`, `END`, and `ABORT` lifecycle
records. Independent event types and phases may overlap. Historical records
carry the evaluated policy and generation and never depend on current settings.

## Private PL/R5C1 packets and public records

Private packets are exact CRC32C-protected co-release contracts with a
four-word header, one revision word, fixed payload, and trailing CRC:

| Packet | Magic | Payload | Frame |
| --- | ---: | ---: | ---: |
| PQE1 | `0x31455150` | 64 words | 69 words |
| FLK1 | `0x314B4C46` | 64 words | 69 words |
| MCS1 | `0x3153434D` | 20 words | 25 words |

The generalized arbiter grants only at packet boundaries, never interleaves
families, prioritizes AGG1 and active-event traffic, and gives the remaining
families bounded round-robin service so HRM1 cannot starve. Malformed frames
are discarded by family before they can enter an engine.

R5C1 returns finalized `PQ-EVENT-v1` (`0x00060001`), `FLICKER-v1`
(`0x000E0001`), and `MAINS-SIGNAL-v1` (`0x000F0001`) records through the one
existing S02 output. Low-level M12 `PQEVT-v1` diagnostics remain direct on S01.
Sample data and measurement records never use RPMsg.

## Product measurement surfaces

Acquisition IPC v38 exposes the three independent latest FLICKER-v1 views, the
latest MAINS-SIGNAL-v1 observation, and R5C1 control/input/output/validator
runtime stack high-water headroom. The existing
`meter-power-quality` command remains the M12 Urms(1/2) diagnostic; it is not
silently redefined as the M18 lifecycle catalogue.

The durable catalogue is queried through meter-historian IPC by stable
canonical event UUID. Authenticated viewers use
`GET /api/v1/meter/power-quality/events`, optionally filtered by canonical
`event_id`, UTC bounds, and a bounded limit. `GET /api/v1/meter/flicker` and
`GET /api/v1/meter/mains-signalling` return engineering-unit projections of
the typed acquisition snapshots. Equivalent local commands are
`mnc meter power-quality events`, `mnc meter flicker`, and
`mnc meter mains-signalling`; all support the normal JSON envelope.

An administrator may delete selected canonical event UUIDs, or explicitly
confirm a complete catalogue clear, through
`DELETE /api/v1/meter/power-quality/events`. Deleting an event removes its
catalogue-owned evidence links but deliberately does not delete shared MNCWF
files. Waveform deletion is a separate capture-storage operation.

When waveform policy is enabled, acquisition assigns the capture UUID before
queuing the session and sends the event/capture association to the historian
on a separate ordered retry worker. The catalogue may therefore lag the live
edge briefly, but historian unavailability never blocks the DMA ingestion
thread. Duplicate lifecycle updates and overlapping-event links are
idempotent. One event can list several continuation capture UUIDs and one
capture UUID can appear on several overlapping events.

Acquisition IPC session summaries include a bit mask of all trigger sources
that contributed to a possibly merged capture. The web projection reports
`manual`, `power_quality`, `mixed`, or `legacy`. The waveform library defaults
to all origins and supports manual and power-quality filters without splitting
the underlying capture or storage implementation. Mixed sessions belong to
both filtered views; legacy sessions belong only to the all-origin view.

## Simulator 1.5

The simulator accepts absolute frequencies from settings. R5C0 converts each
frequency to a rounded Q0.32 phase step for the active sample rate; the PL owns
separate fundamental, AM, carrier, and adjacent-tone accumulators. APPLY with
preserve-phase set retains every accumulator. A restarting APPLY clears all of
them.

AM is `1 + depth × sin(am_phase)` on the selected lanes. The carrier and
adjacent interferer are independent absolute-frequency tones, use a selectable
voltage-phase mask, and express magnitude as a Q16 fraction of that lane's
configured fundamental peak. Disabled controls are all-zero and bit inert.

## Persistent and export boundary

MNCWF v4 is the M18 persistent-waveform baseline. The current writer emits
MNCWF v5, which keeps every v4 metadata field and replaces only sample section
v1 with bounded raw/Zstd chunks. Product readers and exports accept both v4
and v5; existing masters are never recompressed. MNCWF remains the sole
recording format. The post-capture ProtocolGateway converter emits IEC
60255-24:2013 CFF/BINARY32, a legacy ZIP containing CFG and DAT, or IEEE
1159.3-2025 PQDIF without consulting mutable live state. Missing source
authority and discontinuous selections fail explicitly rather than producing
a lossy placeholder.

Authenticated viewers request an event slice with
`GET /api/v1/waveforms/export?session_id=...&event_id=...&format=mncwf`.
Local operators use `mnc waveform export --session ... --event ... --format
mncwf|comtrade|comtrade-zip|pqdif [--file ...]`; omitting `--event` selects the
complete capture. Both resolve completed sessions by exact full-archive
lookup, including sessions older than the newest catalogue page. The shared
read-only exporter validates and maps the v4/v5 master, regenerates the
virtual slice metadata and integrity fields, copies complete v5 chunks, and
rebuilds only partial boundary chunks without another persistent on-device
capture.

Converted REST exports use `/api/v1/waveform-exports`: POST submits an
owner-scoped asynchronous job, GET polls it, DELETE cancels/discards it, and
`/download` streams a ready artifact through the authenticated backend. The
converter is optional to Web availability; an unhealthy service leaves the
legacy MNCWF paths intact and removes only converted capability entries.
