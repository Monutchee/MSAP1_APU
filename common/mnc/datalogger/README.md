# Reusable meter Datalogger

This directory is the product-neutral M19 library for turning historical meter
points into deterministic, durable artifacts. A product integrates it by
providing `HistoricalMeterDataSource`, product/device identity, an
`OutboxRepository`, a clock, and any required `DataChannel` implementations.
No MSAP1 service, WebEngine, settings, DMA, RPMsg, or historian storage detail
belongs in this layer.

The dependency direction is:

```text
types and canonical meter attributes
    -> reusable Datalogger, aggregators, writers, scheduler, outbox, channels
    -> product historian/settings adapters
    -> product Data Sender service and external API
```

`Datalogger` divides one completed UTC artifact window into ordered half-open
row windows. It requests typed points from the injected history source and
applies only calculations allowed by the canonical attribute descriptor:
linear min/max/average/last, circular angle average/last, exact signed 64-bit
counter first/last/delta, and categorical/peak last. Only valid samples
contribute. Empty and incomplete coverage remain explicit; valid zero is never
treated as missing, and a counter reset epoch makes delta unavailable.

`MeterDataContentWriter` is a strategy interface. The JSON and RFC 4180 CSV
writers serialize the same ordered typed dataset using the
`mnc.meter.datalog.v1` contract. Artifact IDs and safe filenames derive from
job ID, immutable job revision, UTC start/end, and format. Integer values never
pass through floating point. Final payload publication uses a flushed,
fsynced temporary file, atomic rename, and parent-directory fsync.

The SQLite outbox stores immutable artifact metadata and one independent
delivery row for each selected channel. Delivery is at least once: a receiver
of an HTTP POST must deduplicate the stable idempotency key when it commits a
request but its response is lost. FTP/SFTP use deterministic final names for
the same reason. A remote payload is removed only after every selected channel
has durably succeeded; completed metadata is retained for the configured
period. Local-only artifacts have no delivery rows and remain in the archive
until an administrator deletes them.

Quota and minimum-free-space guards pause generation while preserving unsent
payloads and eligible retries. Startup recovery inventories temporary files,
payloads, and manifest rows; a missing or damaged payload becomes explicit
critical health and is never interpreted as successful delivery.

The scheduler stores per-job watermarks, waits for completed UTC boundaries,
and retries only pending/failed channel rows with persisted exponential
backoff. Editing a job creates a new immutable revision at the next boundary;
queued artifacts retain their original content and channel selection.

When a host build finds libcurl, it also builds
`data_channel_integration_probe`. Run it through
`tests/integration/run_data_channel_endpoints.py <probe-path>`; the harness
creates isolated loopback HTTP, mutually authenticated HTTPS, FTP, and SFTP
servers and covers success, retry/rejection, credential/trust failure, stable
HTTP identity headers, and temporary-upload/final-rename behavior. It needs no
external service and never uses production credentials.
