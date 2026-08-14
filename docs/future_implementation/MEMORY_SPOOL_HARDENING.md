# Memory-Backed Spool Hardening

Status: implemented 2026-08-14 (all steps below, plus the appendix's ring
deepening at 8 records and meter-path transport-overrun logging; the WAL
checkpoint bug found during the same investigation — a row-active statement
across COMMIT in `MeterHistoryStore::append()` — is fixed alongside, with a
regression test in `tests/meter_stream_test.cpp`).
Origin: 2026-08-13 `meter_sequence_gap` incident investigation.

## Background

Incident (2026-08-13, 16:59–17:39 UTC): the acquisition daemon lost one basic
MTR1 record per 3 s aggregate window (~796 records). Root cause chain:

1. Every accepted PL record is published through a synchronous blocking IPC
   (`MeterRecordIngestor::accept` → `MeterRecordStreamClient::publish`) to
   msap1-meter-stream.
2. `DurableMeterSpool` commits each record with `PRAGMA synchronous=FULL`;
   with the persistent backend that is a full fsync on `/data` per record.
3. `/data` is a removable SD card. During a ~40-minute write-latency episode
   (aggravated by the historian's multi-GB un-checkpointed WAL on the same
   card) each publish round-trip exceeded the ~600 ms tolerance of the
   4-record `/dev/msap1-meter` kernel DMA ring, and `msap1_dma_catch_up()`
   silently dropped the oldest period. The meter path never queries
   `MSAP1_DMA_IOC_TRANSPORT_STATUS`, so kernel-side loss is visible only as
   wire sequence gaps.

Switching the spool dataset (`raw_record_spool`) to `StorageBackend::memory`
removes every `/data` touch from the publish round trip (verified: the WAL
pragma is skipped for `:memory:`, `synchronous=FULL` is a no-op, and
`sqlite_family_size()` in `status()` is gated on durability). Steady-state
RAM is ~7–8 MB at the default 1 h retention.

A bare backend flip is NOT safe. Three restart-semantics assumptions hold
today only because the spool is persistent:

1. **Cursor reuse.** A fresh `:memory:` spool restarts `AUTOINCREMENT` at 1.
   The historian dedups on `stream_cursor INTEGER NOT NULL UNIQUE` +
   `INSERT OR IGNORE` (`meter_history.cpp`) and keeps persisted
   `clear_through_cursor` floors, so after every reboot new measurements are
   silently discarded from the persistent datasets until the cursor passes
   its historical maximum.
2. **Historian wedge.** The `consumers` table is volatile too. After a
   meter-stream-only restart, `read_after` throws
   "meter stream consumer is not registered"; the historian's consume loop
   retries forever without re-registering, ingestion stops, and the uncapped
   spool grows without bound (OOM kills meter-stream, which tears down
   acquisition because `publish()` failures propagate).
3. **Unreported loss.** An empty spool reports `oldest_cursor = 0`
   (`COALESCE(MIN(cursor),0)`), which defeats the historian's
   `backfill_incomplete = oldest_cursor > 1` heuristic — data lost at a
   power cut would be invisible.

All paths below are relative to `applications/MSAP1_APU/`.

## Plan

### 1. Cursor lease (fixes bug 1) — `common/mnc/MeterDataProvider/stream/durable_meter_spool.cpp/.hpp`

Persist a cursor high-water lease next to the spool path:
`/data/mnc/database/meter-stream/spool.sqlite3.cursor-lease` (ASCII uint64).

- In `Impl::initialize()`:
  `seed = max(lease, MAX(cursor) in DB, lease-missing ? now_ms : 0)`; seed
  `sqlite_sequence` (`UPDATE ... SET seq=? WHERE name='records' AND seq<?`,
  INSERT the row if absent); atomically write `lease = seed + 2^31`
  (temp + fsync + rename, pattern from
  `common/mnc/settings/settings_repository.cpp`); record
  `session_start_cursor_ = seed`.
- Renew the lease in `prune()` (runs on the acknowledge path — historian
  thread, never acquisition) when `newest_cursor > lease - 2^30`.
- Corrupt/unwritable lease: log and proceed with the computed seed; the
  clock bootstrap covers the next session. `create_directories` on the
  parent, since a memory spool never opens the sqlite file itself.

Rationale: monotonic cursors are assumed by the historian dedup key, its
clear floors, acknowledged-cursor recovery, and the web lag computation.
Changing the dedup key instead would still trip `stream_cursor UNIQUE` and
strand records below persisted clear floors. Clock-only seeding fails on RTC
loss or a backwards step; it is used solely as the first-boot bootstrap
(milliseconds-since-epoch is orders of magnitude above any count-based cursor
a deployed device can have, so upgrades are collision-free). Cost: one small
fsync at meter-stream startup; the 2^31 reservation (~11 years at ~6
records/s) means no per-record or periodic I/O.

### 2. StreamStatus wire extension (carries fixes 3 and 4)

- `common/mnc/MeterDataProvider/stream/meter_stream_status.hpp`: add
  `std::uint64_t session_start_cursor = 0;` and
  `std::uint64_t dropped_unacknowledged_records = 0;`.
- Encode in `apps/MeterCore/Services/meter-stream/meter_stream_service.cpp`
  (two u64 after `storage_bytes`, before the consumer list); decode
  symmetrically in
  `common/msap1/meter/MeterDataProvider/stream/meter_stream_ipc.cpp`; bump
  `protocol_version` to 2 in `meter_stream_ipc.hpp` (documentation constant —
  it is not on the wire; `require_finished()` makes any accidental version
  mix fail loudly, and all peers ship in one image).

### 3. Hard byte cap (fixes the OOM half of bug 2) — `durable_meter_spool.cpp`

Keep an O(1) running byte counter (`SUM(LENGTH(payload)+128)` seeded once per
`Impl`, adjusted on insert/delete). In `publish()`, when over
`maximum_bytes`: delete oldest rows until under the cap — acknowledged rows
first, then unacknowledged, counting the latter in an atomic
`dropped_unacknowledged_records_` surfaced via `StreamStatus`. The
publish handler in `meter_stream_service.cpp` emits a rate-limited warning
(`spool_records_dropped`) when the counter grows.

Enforcement must live in `publish()` because `prune()` only runs from the
acknowledge handler — absent in exactly the wedged-consumer scenario.
Bounded, logged loss beats an OOM-kill of meter-stream.

### 4. Historian consumer re-registration (fixes bug 2) — `apps/MeterCore/Services/meter-historian/meter_historian_service.cpp`

In the consume-loop catch block, before the existing 1 s sleep: best-effort
`stream_.register_consumer("historian")` with log tag
`historian_consumer_reregistered`. Registration is already idempotent
(`ON CONFLICT(name) DO UPDATE` preserves `acknowledged_cursor`), so no
error-string matching is needed. After a meter-stream restart the fresh
consumer starts at cursor 0 and replays the new session's spool once; the
replay is idempotent for all datasets because both historian databases carry
`stream_cursor UNIQUE` + `INSERT OR IGNORE`, and fix 1 guarantees replayed
cursors are genuinely new.

### 5. Truthful backfill detection (fixes bug 3)

- `common/msap1/meter/history/meter_history.hpp/.cpp`: add
  `persisted_stream_high_water()` (persistent-DB `MAX(stream_cursor)`,
  mirroring the existing startup query).
- `meter_historian_service.cpp`: replace `oldest_cursor > 1` with a testable
  free-function predicate:
  - virgin historian (`persisted_max == 0`): incomplete iff
    `oldest_cursor > session_start_cursor + 1` (records pruned within this
    spool session);
  - otherwise: incomplete iff `oldest_cursor > persisted_max + 1` (the spool
    no longer covers everything past durable coverage — truthfully true
    after a memory-spool reboot, false for a persistent spool that retained
    coverage).
- `apps/MeterCore/Services/web-backend/api/database_routes.cpp`: additively
  expose `session_start_cursor` and `dropped_unacknowledged_records` in the
  status DTO.

### 6. Defaults switch — factory default becomes memory

- `config/settings/factory-defaults.json` →
  `"spool": {"backend": "memory", "maximum_age_seconds": 3600, "maximum_bytes": 33554432, "volatile_spool_acknowledged": true}`.
- `common/msap1/settings/definition/database_settings.hpp`: matching member
  defaults (the comment there requires they mirror the JSON); extend the
  rationale comment.
- Keep the `volatile_spool_acknowledged` validation gate unchanged.
- Existing devices keep their persisted settings document; switch them live
  via `PUT /api/v1/developer/database` — `apply_storage_policy` →
  `DurableMeterSpool::apply_policy` migrates records and consumer cursors
  without a restart.
- No acquisition-daemon, kernel, or PL changes in this plan.

### 7. Tests — extend `tests/meter_stream_test.cpp`

- `memory_spool_cursors_survive_restart` (including corrupt-lease fallback
  staying monotonic; lease file created).
- `register_consumer_preserves_acknowledged_cursor`.
- `publish_enforces_hard_byte_cap` (bounded count, dropped counter grows,
  `read_after` returns ordered survivors).
- `session_start_cursor_reported_and_advances_across_restart`; `apply_policy`
  round-trip keeps monotonicity and re-reserves the lease.
- `backfill_incomplete_predicate` table-driven: virgin+fresh (complete),
  pruned-in-session (incomplete), spool restarted past persisted high-water
  (incomplete), persistent spool with coverage (complete).
- Re-run `settings_test` for the defaults change.

## Verification

1. Host build + `ctest` (cross-build tests run on x86_64).
2. On the target device:
   - `PUT /api/v1/developer/database` spool→memory: response shows
     `durability=false`, `session_start_cursor` set, no service restarts.
   - `systemctl restart msap1-meter-stream` alone: journal shows
     `historian_consumer_reregistered`; historian `acknowledged_cursor`
     advances; cursors jump forward, never backward.
   - Reboot: new persistent-dataset rows appear with `stream_cursor` above
     the pre-reboot maximum (nothing swallowed by `INSERT OR IGNORE`);
     `backfill_incomplete=true` is reported (truthful volatile-window loss).
   - Wedge test: stop the historian, let the spool exceed 32 MiB → RSS
     plateaus, rate-limited `spool_records_dropped` warnings; restart the
     historian → recovery.
   - SD-latency A/B: sustained `/data` write pressure; expect zero
     `meter_sequence_gap` with the memory spool, reproducible with
     persistent.

## Appendix: should the kernel meter DMA ring also be deepened?

Separate change (kernel module `msap1-dma`, `MSAP1_METER_RING_RECORDS` in
`msap1_dma_meter.c`), not required for this plan, but worth doing as
defense-in-depth.

The latency concern is narrower than it first appears. The ring depth does
**not** add steady-state latency: in cyclic mode the DMA driver raises a
callback per completed period, the reader is woken immediately, and each
record is delivered as soon as its 256-byte period lands — regardless of how
many ring slots exist behind it. The only cost is **first-record latency
after capture start**: the Xilinx AXI DMAengine driver raises its first
cyclic callback only after the final descriptor of the ring completes once,
so the first record appears after `ring_periods × cadence` (documented in the
sizing-history comment in `msap1_dma_meter.c`; this is why the original
64-record ring — 12.8 s to first record — was shrunk).

| Ring records | First record after capture start | Stall tolerance |
|---|---|---|
| 4 (today) | ~0.8 s | ~0.6 s |
| 8 | ~1.6 s | ~1.4 s |
| 16 | ~3.2 s | ~3.0 s |

Capture start happens at boot and on acquisition reconfiguration, so the
first-record delay is a startup/reconfig UX cost, not a measurement latency.
With the memory spool in place the publish round trip is sub-millisecond and
the ring only needs to absorb scheduler jitter; 8 records (~1.4 s tolerance,
~1.6 s startup) is a balanced choice. Pair it with reading
`MSAP1_DMA_IOC_TRANSPORT_STATUS` in the meter path and logging
`overrun_blocks`, so kernel-side loss is directly observable instead of
inferred from wire sequence gaps.
