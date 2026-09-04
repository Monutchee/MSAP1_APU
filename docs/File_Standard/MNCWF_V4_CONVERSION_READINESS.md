# MNCWF v4/v5 COMTRADE and PQDIF source readiness

## Decision and scope

MNCWF v4 defines the conversion metadata contract and MNCWF v5 preserves it
byte-for-byte while compressing only sample storage. The product does **not**
ship a COMTRADE or PQDIF writer and must advertise only `mncwf` as an export
format. Later gateway converters consume one validated MNCWF v4 or v5 stream;
they do not
query live settings, the event historian, the sensor-profile database, or
device identity services.

This matrix was checked on 2026-08-29 against:

- [IEC 60255-24:2013](https://webstore.iec.ch/en/publication/1170), whose IEC
  record identifies CFG, DAT, HDR, and INF information and the combined CFF
  form; and
- [IEEE 1159.3-2025](https://standards.ieee.org/ieee/1159.3/10437/) plus its
  [versioned normative logical, physical, and ID
  definitions](https://opensource.ieee.org/pqdif/pqdif-normative/-/tree/1.0.0).

The IEEE normative model organizes PQDIF as Container, Data Source, optional
Monitor Settings, and Observation records, with channel definitions and
series definitions/instances below them. Its public identifiers are used by
name below so a future implementation can be reviewed mechanically.

“Ready” means the source value is explicit in MNCWF v4/v5 or can be derived
deterministically from that file and this format specification. It does not
mean a destination encoder exists, and it is not an IEC/IEEE conformance
claim. Destination syntax, identifier tables, rounding, output splitting, and
certification vectors remain converter work.

## Executable readiness gate

`assess_mncwf_v4_conversion_readiness()` returns separate missing-field lists
for full-fidelity COMTRADE and PQDIF event export. A file is ready only when:

- station, site, circuit, device model, serial, nominal voltage, and nominal
  frequency are captured rather than inferred later;
- every timebase segment has UTC context, TAI correlation, and known time
  quality;
- every channel has phase, quantity, exact affine conversion,
  primary/secondary ratio, range, and clipping metadata;
- PQDIF additionally has topology, calibration ID, clock source, channel
  nominal/resolution/calibration validity; and
- at least one typed event exists, every event has taxonomy, TAI/UTC anchors,
  and its evaluated settings snapshot, while COMTRADE also requires an exact
  trigger sequence.

The binary reader's structural validation runs first. The readiness gate never
repairs an incomplete file and never reads current device state. The test
fixture proves both ready output and field-specific failure reporting.

## COMTRADE CFG readiness

| Destination concept/field group | MNCWF v4 authority | Status and conversion rule |
|---|---|---|
| station name | capture `station_name` | Ready, direct; blank fails readiness |
| recording-device identity | product, model, serial, device UUID, firmware/build | Ready; converter formats a stable `rec_dev_id` from captured values |
| revision year | converter profile | Converter-owned constant (`2013`), not measurement state |
| total/analog/status channel counts | channel definitions plus selected event-active projections | Ready, derived before CFG is emitted |
| analog channel number/order | channel record order | Ready; record order is sample-frame order |
| channel ID | name, stable channel UUID, source channel | Ready; name is human ID and UUID/source preserve identity |
| phase | channel phase enum | Ready, direct mapping including neutral and line-to-line phases |
| circuit/component identifier | capture circuit and channel description | Ready, direct |
| engineering unit | SI-unit enum, unit symbol, display exponent | Ready; converter uses the captured SI authority |
| analog multiplier/addend | exact gain and offset rationals | Ready; destination `a`/`b` are derived without current calibration data |
| channel skew | v4 simultaneous-frame invariant | Ready, deterministic zero for every stored channel |
| minimum/maximum | characterized range, raw clipping bounds, stored/valid bits | Ready; integer DAT uses raw bounds and converted forms use the exact affine transform |
| primary/secondary values and P/S indicator | exact primary/secondary ratio | Ready; numerator/denominator preserve the ratio, with policy selecting the displayed side |
| status-channel names | event UUID, taxonomy name, label, phase mask | Ready, derived; no sampled status payload is invented |
| nominal line frequency | exact capture nominal-frequency rational | Ready, direct |
| number of sample-rate segments | timebase segments | Ready; adjacent identical segments may be coalesced deterministically |
| sample rate and terminal sample number per segment | persisted-rate rational, first frame, frame count | Ready; destination decimal formatting is converter-owned |
| recording start timestamp | timebase UTC correlation and exact sequence/rate mapping | Ready, derived with declared rounding |
| trigger timestamp | event trigger sequence plus UTC anchor/correlation | Ready; an absent trigger fails COMTRADE readiness |
| data encoding (`ASCII`, `BINARY`, `BINARY32`, `FLOAT32`) | sample encoding, stored bits, affine metadata | Ready for converter choice; no encoding preference is persisted |
| timestamp multiplier | exact rational rate/correlation | Ready; converter chooses an integer tick unit that preserves the requested output |
| time code/local code | UTC anchor and active UTC-to-local offset | Ready; format as UTC and captured numeric local offset, never host timezone |
| time quality and leap context | time quality, uncertainty, leap flags | Ready, direct policy mapping |

## COMTRADE DAT, HDR, INF, and CFF readiness

| Destination region | Destination content | MNCWF v4 authority | Status |
|---|---|---|---|
| DAT | sample number | persisted frame index and timebase segment | Ready, derived |
| DAT | per-sample timestamp | sequence/rate segments plus UTC correlation | Ready, derived using integer/rational arithmetic |
| DAT | analog sample values | signed interleaved sample section | Ready, direct or exactly affinely converted |
| DAT | digital/status values | event start/end anchors and lifecycle | Ready, deterministic projection; labelled derived rather than sampled |
| DAT | raw versus decimated semantics | decimation method/divisor and exact source-frame count | Ready; short final boxcar group is unambiguous |
| DAT | rate changes | explicit segment geometry and rate-change flag | Ready |
| DAT | missing/corrupt intervals | sequence gaps and quality intervals | Source-ready; a converter must explicitly split, mark, or reject a destination representation that cannot preserve the gap |
| HDR | human description | station/site/circuit, product/model, comments | Ready |
| HDR | event summary | UUID, taxonomy, lifecycle, phase, severity, thresholds, extrema, duration | Ready |
| HDR | clock/calibration/quality disclosure | time quality/uncertainty, calibration status/ID, quality intervals | Ready |
| INF | machine-readable source/config identity | capture/device/channel UUIDs, configuration and sensor SHA-256 values, firmware/build IDs | Ready |
| INF | evaluated event settings | configuration generation and settings-snapshot JSON | Ready; never reconstructed from current settings |
| INF | continuation/parent links | lineage entries and capture/event UUIDs | Ready |
| CFF | combined CFG/INF/HDR/DAT packaging | all regions above | Source-ready; framing and destination integrity are future converter work |

For event-active status projection, one selected event channel is high from
its start through its valid end sequence, inclusive. An event without a valid
end remains high through the final source sequence in that master and is
labelled incomplete/contaminated as applicable. Selection and naming are
stable functions of event UUID and descriptor content.

## PQDIF Container readiness

| PQDIF tag/concept | MNCWF v4 authority | Status and rule |
|---|---|---|
| record/file signatures and `tagVersionInfo` | converter implementation | Converter-owned normative constants |
| `tagFileName` | capture UUID and export naming policy | Ready, deterministic |
| `tagCreation`, `tagLastSaved`, `tagTimesSaved` | captured creation UTC/TAI | Ready; an immutable export has one save |
| `tagTitle`, `tagSubject` | station/circuit and event label/taxonomy | Ready, derived |
| `tagComments` | capture comments plus quality/lineage disclosure | Ready |
| language/author/keywords/contact fields | no measurement dependency | Optional; converter may use fixed neutral values or omit them, never query the device |

## PQDIF Data Source and channel-definition readiness

| PQDIF tag/concept | MNCWF v4 authority | Status and rule |
|---|---|---|
| `tagDataSourceTypeID` | product name/model and converter namespace | Ready; deterministic standard/custom GUID mapping |
| `tagVendorID` | no third-party branding in product records | Optional and intentionally omitted |
| `tagEquipmentID` | device model and stable device UUID | Ready, deterministic GUID mapping |
| `tagCustomSourceInfo` | product, firmware, build, hashes | Ready |
| `tagSerialNumberDS` | captured device serial | Ready, direct |
| `tagVersionDS` | firmware and software build | Ready, direct |
| `tagNameDS` | product/device model | Ready, direct |
| `tagOwnerDS` | not a waveform measurement | Optional and intentionally omitted |
| `tagLocationDS` | station, site, circuit | Ready, direct |
| `tagTimeZoneDS`, `tagUTCtoLST` | active UTC-to-local offset | Ready; deterministic `UTC±hh:mm` text and numeric offset |
| coordinates/latitude/longitude | not configured in M18 | Optional and intentionally omitted, never externally looked up |
| `tagInstrumentTypeID` | product instrument class | Ready, converter mapping |
| `tagInstrumentModelName/Number` | captured device model | Ready |
| `tagChannelDefns` / `tagOneChannelDefn` | channel records | Ready, one definition per stored channel |
| `tagChannelName` | channel name | Ready, direct |
| `tagPhaseID` | phase enum | Ready, direct mapping |
| `tagOtherChannelIdentifier` | stable channel UUID and source channel | Ready |
| `tagGroupName` | station/circuit and channel description | Ready |
| `tagQuantityTypeID` | quantity enum and sample-series shape | Ready, converter ID mapping |
| `tagQuantityMeasuredID` | current/voltage/status/frequency/ratio enum | Ready, direct mapping |
| `tagPhysicalChannel` | source-channel number | Ready, direct |
| `tagQuantityName` | channel description | Ready |
| `tagPrimarySeriesIdx` | generated series-definition order | Ready, derived |
| `tagSeriesDefns` / `tagOneSeriesDefn` | sample, time, and optional quality series plan | Ready, converter-generated |
| `tagQuantityUnitsID` | SI unit and display exponent | Ready, mapped without guessing |
| `tagQuantityCharacteristicID` | waveform value/time/flagged-interval role | Ready, converter mapping |
| significant digits/resolution | valid bits and exact resolution rational | Ready |
| preferred units | unit symbol/display exponent | Ready |
| `tagSeriesNominalQuantity` | per-channel nominal or capture nominal voltage | Ready |

PQDIF uses GUIDs for extensibility. The converter owns stable namespace and
standard-ID lookup tables; capture/device/channel/event UUIDs supply persistent
element identity but are never substituted blindly for a PQDIF semantic ID.

## PQDIF Monitor Settings readiness

| PQDIF tag/concept | MNCWF v4 authority | Status and rule |
|---|---|---|
| monitor-settings record identity/effective time | configuration SHA-256/ID/generation and creation/event times | Ready |
| `tagUseCalibration` | capture and channel calibration status/flags | Ready |
| `tagNominalFrequency` | exact nominal-frequency rational | Ready |
| `tagChannelSettingsArray`, `tagOneChannelSetting`, `tagChannelDefnIdx` | channel order and stable IDs | Ready |
| trigger type/high/low/deadband fields | typed event descriptor plus exact evaluated settings JSON | Ready; schema-aware converter reads the captured snapshot |
| full scale | exact characterized range | Ready |
| transformer type and system/monitor ratio | quantity plus exact primary/secondary ratio | Ready |
| calibration offset/ratio | exact affine transform and calibration validity | Ready for the applied conversion |
| calibration arrays, time skew, noise floor, transducer frequency response | not required to reproduce stored samples; simultaneous skew is zero | Optional; omit unsupported characterization instead of inventing it |

The JSON snapshot is retained because future event schemas may contain
standard-specific settings that do not belong in a fixed waveform header. Its
configuration generation and SHA-256 bind it to the capture.

## PQDIF Observation and series-instance readiness

| PQDIF tag/concept | MNCWF v4 authority | Status and rule |
|---|---|---|
| `tagObservationName` | event label/taxonomy and capture UUID | Ready |
| `tagTimeCreate` | captured creation UTC | Ready |
| `tagTimeStart` | first timebase sequence and UTC correlation | Ready, derived |
| `tagTriggerMethodID` | trigger source and event taxonomy | Ready, mapped |
| `tagTimeTriggered` | event trigger UTC/sequence | Ready when present |
| `tagChannelTriggerIdx` | event phase/quantity mapped to channel definitions | Ready, derived |
| observation identity/serial | event and capture UUIDs | Ready; retain UUID in extensible metadata and use an optional deterministic serial only if required by profile |
| disturbance category | IEC/product taxonomy and event type | Ready, mapping table is converter work |
| `tagChannelInstances` / `tagOneChannelInst` | selected channel definitions | Ready |
| duration/magnitude/frequency characterizations | duration, extrema/reference/threshold and nominal frequency | Ready where meaningful; omit unrelated optional fields |
| `tagSeriesInstances` / `tagOneSeriesInstance` | sample/time/quality series generated from sections | Ready |
| `tagSeriesBaseQuantity` | channel/capture nominal | Ready |
| `tagSeriesScale`, `tagSeriesOffset` | exact affine gain and offset | Ready |
| `tagSeriesValues` | signed sample payload | Ready, direct |
| regular-rate/time series | exact persisted-rate segments and UTC correlation | Ready; shared time series is deterministic |
| rate changes | multiple timebase segments | Ready; choose normative multi-rate or explicit-time representation |
| flagged intervals | quality section flags/ranges/channel mask | Ready; map known quality types and preserve product detail in extensible metadata |
| event settings and characterization | event descriptor and settings JSON | Ready |
| parent/continuation/virtual-slice identity | lineage UUIDs, sequence ranges, part geometry | Ready through deterministic extensible metadata |

## Information intentionally not fabricated

Some destination fields are optional characterization or administrative
metadata, not prerequisites for representing the captured waveform/event.
MNCWF v4 intentionally does not invent geographic coordinates, owner/contact
details, third-party vendor branding, noise floor, transducer frequency
response, calibration stimulus arrays, disturbance direction, or fault
location. A future converter omits these fields unless a later MNCWF version
captures them. It must never fetch them from a mutable external database while
exporting an old event.

## Converter implementation gates for the later milestone

Before advertising either format, the later gateway milestone must:

1. accept only a structurally valid MNCWF v4 whose appropriate readiness list
   is empty;
2. pin destination revision/profile and identifier mappings;
3. use checked integer/rational conversion and documented rounding;
4. prove raw, boxcar-decimated, short-final-group, multi-rate, gap, clipping,
   overlapping-event, and continuation vectors;
5. define explicit split/reject behavior when one destination file cannot
   represent a source discontinuity without loss;
6. stream output instead of buffering a large derivative in memory;
7. validate generated files with independent COMTRADE/PQDIF readers and
   retained golden vectors; and
8. add the format to advertised export capabilities only after those gates
   pass. Until then, unavailable formats are rejected explicitly.
