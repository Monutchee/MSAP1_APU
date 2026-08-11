# Scaling the history graph to long time ranges

This is a deferred design note, not an implemented contract. It records why
the History page cannot serve a long time range today, what the industry
does about it, and the specific sequence of changes that would fix it here.

The relevant code is:

```text
common/msap1/meter/history/meter_history.cpp   MeterHistoryStore::query()
common/msap1/meter/history/meter_history.hpp   HistoryQuery, HistoryPoint
apps/web-backend/api/history_routes.cpp        maximum_points = 50000
MSAP1_WEB/src/history/HistoryPage.tsx          the chart and its paging
MSAP1_WEB/src/api.ts                           HistoryQuery/HistoryResponse
```

## The problem

`HistoryQuery` returns one point per (block, attribute) and the response is
bounded — 10 000 points per page from the web client, 50 000 by the route.
When the range holds more than that, the client shows *"The query reached its
bounded page size"* and offers to fetch the next page.

At the two measurement tiers that are actually populated today, with three
attributes selected:

| Range | `basic` points | JSON (~130 B/point) | Pages @10k | `cycles_150_180` points |
| --- | --- | --- | --- | --- |
| 10 minutes | 9 000 | 1.2 MB | 1 | 600 |
| 2 hours | 108 000 | 14 MB | 11 | 7 200 |
| 1 day | 1 296 000 | 168 MB | 130 | 86 400 |
| 1 week | 9 072 000 | 1.2 GB | 907 | 604 800 |
| 30 days | 38 880 000 | ~5 GB | 3 888 | 2 592 000 |

A month is not servable at either tier by paging. Note also that `basic` is
memory-backed with a 24 h retention, so month-scale history only exists in
`cycles_150_180` regardless.

## The constraint that resolves it

A 1 900 px wide chart can render about 1 900 distinct x positions. There is a
hard ceiling of roughly **four numbers per pixel column per series** that a
line chart can express, whatever it is fed.

So the front end does not need the data, it needs the picture. Reducing on
the server does not discard information the display could have shown; it
discards information that was already invisible. Detail the operator wants
reappears on zoom, because zoom becomes a query at finer resolution.

Everything below follows from that: **never transfer more points than pixels,
and make zoom a query.**

## What industry does

Four strategies, usually combined:

1. **Time-bucket aggregation.** Group by bucket, return min/max/avg/count.
   Grafana's `$__interval`, InfluxDB `aggregateWindow()`, TimescaleDB
   `time_bucket()`, Prometheus step. Rendered as a min–max band with the mean
   over it.
2. **Precomputed rollups.** Materialise coarse tiers once and query the tier
   matching the span: TimescaleDB continuous aggregates, InfluxDB
   downsampling tasks, Prometheus recording rules, ClickHouse
   `AggregatingMergeTree`. The ancestor is **RRDtool**, which keeps several
   round-robin archives at different resolutions with consolidation
   functions.
3. **Viewport loading.** Fetch only the visible window at viewport
   resolution, re-query debounced on pan/zoom, cache by range.
4. **Client-side efficiency.** Typed arrays instead of object arrays, binary
   transport instead of JSON, canvas/WebGL instead of SVG, parsing in a
   worker.

MSAP1 already has strategy 2 in its architecture. `MeasurementPeriod`
basic → 150/180-cycle → 10-minute → 2-hour is an RRD-style resolution
pyramid that IEC 61000-4-30 defines for us. At a 2 000-point budget each tier
covers:

| Tier | Resolution | Span at 2 000 points |
| --- | --- | --- |
| `basic` | 200 ms | ~7 minutes |
| `cycles_150_180` | 3 s | ~1.7 hours |
| `minutes_10` | 10 min | ~14 days |
| `hours_2` | 2 h | ~167 days |

A month at `hours_2` is 360 points. **The two coarse tiers are declared but
nothing writes them** — there is no producer anywhere in the decode or ingest
path. Until they exist, month-scale views must be decimated at query time.

## Decimation fidelity: prefer M4 over LTTB here

**LTTB** (Largest Triangle Three Buckets) is the popular visual-fidelity
algorithm and is available in most charting stacks. It picks the point per
bucket that maximises triangle area with its neighbours, preserving the
silhouette with very few points. It optimises *shape*, not extremes, so it
can drop a single-sample spike.

That is the wrong trade for this product. On a power-quality instrument the
transient is the measurement; a chart that smooths away a dip is worse than
no chart, because it is confidently wrong.

**M4** (Jugel et al., EDBT 2014) groups by pixel column and returns the
**first, last, min and max** of each column — four points per column. For a
1 px line the rasterised result is identical to plotting every point, so the
picture is provably the same rather than approximately the same. It also
expresses directly in SQL, which matters on SQLite.

"Provably the same picture" is a far easier property to defend on a metrology
product than "looks about right". Prefer M4, or at minimum min/max per
bucket. Do not adopt LTTB for the electrical series.

## Proposed sequence

### 1. Bucketed query (the keystone)

Add an optional bucket or target-column count to `HistoryQuery`, and have
`MeterHistoryStore::query()` aggregate per column when it is set. Sketch
against the existing schema:

```sql
WITH bucketed AS (
  SELECT (b.measured_at_ns - :start) * :columns / (:end - :start + 1)
           AS column_index,
         b.measured_at_ns, v.attribute_id, v.signed_value,
         v.quality, v.source_sequence
  FROM measurement_blocks b
  JOIN measurement_values v ON v.block_id = b.id
  WHERE b.period = :period
    AND b.measured_at_ns BETWEEN :start AND :end
)
SELECT column_index, attribute_id,
       MIN(signed_value), MAX(signed_value),
       MIN(measured_at_ns), MAX(measured_at_ns)
FROM bucketed
GROUP BY column_index, attribute_id
```

Min/max per column is the pragmatic core; full M4 adds the column's first and
last values, which SQLite window functions (`FIRST_VALUE`/`LAST_VALUE` over a
partition, 3.25+) can supply.

A low-churn response shape: keep `HistoryPoint` and emit the min and the max
of each column as two points carrying their own real timestamps. The existing
chart then draws the envelope without any client change, and `truncated`
stops being reachable for display purposes.

The `measurement_blocks_time` index on `(period, measured_at_ns)` already
covers the range scan. Cost needs measuring on target: 30 days of
`cycles_150_180` is ~864 k blocks, and `basic` is in memory while the
persistent tiers are on eMMC.

### 2. Produce `minutes_10` and `hours_2`

Once those tiers are populated, month and year views hit tiny tables and
bucketing becomes an optimisation rather than a necessity. This is the
structural fix; step 1 is what makes long ranges work before it lands.

### 3. Viewport-driven loading

With bucketing in place, replace *Load next page* with a debounced re-query
of the visible window at a resolution derived from the chart's pixel width.
Bounded by construction, because the viewport never exceeds the screen.

**Do not do this before step 1.** Auto-loading over undecimated data is the
version that hangs the browser, and it will look fine in a ten-minute test.

### 4. Binary transport, only if profiled

`HistoryPoint` is an object per point carrying an attribute string — on the
order of 100 B of JS heap against 8 B in a `Float64Array`, and JSON parsing is
often the largest single cost. Worth measuring before assuming.

## Baseline already in place

Client-side accumulation was quadratic and has been fixed (MSAP1_WEB,
`fix/datalog_interval`): the page merge deduplicates only the boundary
timestamp instead of rebuilding a keyed map and re-sorting the whole series,
distinct timestamps are collected in one pass, and the uPlot instance is
created once and fed with `setData()` rather than destroyed and rebuilt per
page. Paging is therefore comfortable at hours-scale, but the aligned-column
rebuild is still linear per page, so this raised the ceiling without removing
it.

uPlot is the right renderer and does not need replacing. Apache ECharts is
the closest batteries-included alternative if built-in `dataZoom` and
sampling ever become preferable to bespoke bucketing.

## Open decisions

- Does the bucket count come from the client (it knows its pixel width) or
  does the server choose from the span? Client-supplied is more honest about
  the display; server-chosen is harder to misuse.
- Should the tier be selected automatically from the span, with the existing
  period dropdown becoming an override? Today a user can pick a
  period/range combination that cannot be served, which is the root of the
  reported confusion.
- How should a bucket's quality be aggregated? A column containing any
  non-valid reading probably must not render as valid.
- Do aggregate-quality semantics need to distinguish "no data in this
  column" from "data present but invalid"? The chart already relies on nulls
  to break the line.

## References

- Jugel, Jerzak, Hackenbroich, Markl. *M4: A Visualization-Oriented Time
  Series Data Aggregation.* EDBT 2014.
- Steinarsson. *Downsampling Time Series for Visual Representation.* MSc
  thesis, University of Iceland, 2013 (LTTB).
- RRDtool round-robin archives and consolidation functions, as the original
  multi-resolution rollup design.
