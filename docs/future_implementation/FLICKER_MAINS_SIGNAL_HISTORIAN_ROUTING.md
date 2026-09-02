# TODO: FLICKER-v1 and MAINS-SIGNAL-v1 historian routing

Status: deferred after the 2026-09-02 M20 target check.

## Problem

Acquisition validates `FLICKER-v1` (`0x000E0001`) and
`MAINS-SIGNAL-v1` (`0x000F0001`) records, then publishes the exact 256-byte
records to the durable meter stream. The historian has neither an explicit
route for these formats nor matching built-in registry decoders, so valid
records fall through as `unsupported meter record format`.

The historian acknowledges and skips them, increments
`undecodable_records`, and emits rate-limited warnings. On the target this
counter grew continuously at approximately the combined Flicker and
mains-signalling cadence. Existing typed datasets, including `seconds_10`,
remain healthy, but the warning is misleading and these two record families
have no historical projection.

## Required follow-up

1. Decide the retention policy for each family:
   - add a typed historian dataset and query API; or
   - declare it latest-only and explicitly acknowledge/ignore valid records.
2. Route both formats in `MeterHistorianService::ingest()` before the generic
   decoder path. Valid records must not increment `undecodable_records`.
3. If retained, add storage policy/settings, typed projection, paging, API,
   and retention tests. Preserve Flicker's independent live, Pst, and Plt
   interval identities and provenance.
4. Keep malformed records on the poison-record path so they are counted and
   skipped without wedging the spool consumer.

## Acceptance

- Valid `FLICKER-v1` and `MAINS-SIGNAL-v1` traffic does not increase the
  undecodable counter or flood the journal.
- Malformed records still increase the counter exactly once.
- The historian cursor advances across restart and its lag remains bounded.
- Existing datasets, especially `seconds_10`, are unchanged.
