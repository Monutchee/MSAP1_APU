# Live target tests

These probes exercise a deployed image and are intentionally not registered
with CTest. They require target services, live meter records, and explicit
timing or configuration setup.

## M15 UTC Basic/150–180-cycle overlap

`m15_utc_overlap_target_test` observes the production meter-stream around a UTC
ten-minute boundary and validates:

- the marked Basic overlaps its predecessor while advancing past it;
- the continuing and synchronized aggregates each contain exactly 15
  consecutive Basic sequences and 150 or 180 cycles;
- the continuing aggregate's contribution-count excess exactly equals the
  Basic overlap;
- the synchronized aggregate has a one-to-one physical span; and
- both aggregate paths retain zero continuity errors.

The test first acknowledges the current stream head for its disposable
consumer. Do not remove this step: replaying a cursor-zero backlog perturbs the
acquisition path and invalidates the result.

Build the probe explicitly from an AArch64 APU build configured with
`BUILD_TESTING=ON`:

```sh
cmake --build <apu-build-directory> --target m15_utc_overlap_target_test
```

Do not copy a native x86 host build to the target. Transfer the AArch64 binary
to a temporary target path. Start it after at least one ordinary Basic record
has been emitted and shortly before the next exact UTC ten-minute boundary;
the duration must extend past the synchronized 150/180-cycle close. For
example:

```sh
/tmp/m15_utc_overlap_target_test 30
```

Run once with the complete system configured for 50 Hz and once for 60 Hz.
Confirm `mnc meter health --full` passes before and after each run. Exit status
zero and a final `PASS` line are required; missing boundary records or any
geometry/continuity mismatch returns nonzero.
