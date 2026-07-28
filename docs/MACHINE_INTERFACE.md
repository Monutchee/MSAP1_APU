# MNC machine-readable diagnostic interface

## Purpose

`mnc` has one command implementation and two presentation strategies:

- `HumanTextGenerator`-style generators preserve concise terminal output.
- `MachineJsonGenerator`-style generators emit stable typed JSON envelopes.

Each command collects its result once, then selects a generator from the global
`--output text|json` option. This prevents human and automated diagnostics from
performing different hardware operations or interpreting different state.

Successful one-shot JSON responses use:

```json
{
  "schema": "mnc.response.v1",
  "success": true,
  "data": {}
}
```

Failures keep stdout machine-readable:

```json
{
  "schema": "mnc.response.v1",
  "success": false,
  "error": {
    "code": "ACCESS_DENIED",
    "message": "command requires operator_control access"
  }
}
```

JSON diagnostic queries return exit status `0` even when the reported device is
unhealthy. Inspect typed health fields instead of treating an unhealthy meter
as a transport failure. Usage, authorization, runtime, and unavailable-service
failures remain nonzero.

| Exit | Meaning |
|---:|---|
| `0` | Query or operation completed; inspect returned health fields |
| `1` | Runtime or protocol failure |
| `2` | Invalid command, option, or requested output |
| `3` | Access denied by the command policy |
| `4` | Acquisition service unavailable or timed out |

## Command and access table

The authoritative table is generated from the same metadata that enforces
restricted execution:

```sh
mnc machine describe
mnc --output json machine describe
```

Access classes are:

| Class | Meaning |
|---|---|
| `diagnostic` | Read-only, bounded state inspection allowed remotely |
| `operator_control` | Changes runtime capture or sample-rate state |
| `maintenance` | Performs an invasive or destructive diagnostic |
| `local_only` | Interactive, continuous, completion, or local tooling |

Option-sensitive commands elevate automatically. For example, `mnc adc rate`
is diagnostic, while adding `--sps` makes it operator control. Similarly,
cached `meter health` is diagnostic, `--refresh` is maintenance, bounded log
queries are diagnostic, and `--follow` is local-only.

## Diagnostic examples

```sh
mnc --output json meter health
mnc --output json meter health --full
mnc --output json meter snapshot
mnc --output json adc rate
mnc --output json system temperature
mnc --output json log --priority warning --limit 100
```

Bounded log pages return an opaque base64url continuation token:

```sh
mnc --output json log --limit 100 --cursor TOKEN
```

The token wraps a journald cursor. Do not parse or modify it.

## Restricted SSH test account

The optional Yocto setting:

```bitbake
MSAP1_ENABLE_DEBUGAI = "1"
```

installs the temporary `debugai`/`debugai` account. It is disabled by default
and forbidden in production-flash builds.

The account has no interactive shell, PTY, forwarding, tunnel, SCP, or SFTP
access. Its forced gateway:

1. reads `SSH_ORIGINAL_COMMAND`;
2. parses it without a shell;
3. requires an `mnc --output json ...` invocation;
4. rejects socket and timeout overrides;
5. enforces the command metadata at diagnostic access.

Example:

```sh
ssh debugai@METER \
  'mnc --output json meter health'
ssh debugai@METER \
  'mnc --output json log --priority warning --limit 50'
```

Commands such as `adc start`, `adc stop`, `adc rate --sps`, `meter health
--refresh`, `adc testflw`, and `log --follow` are rejected before their handlers
run.

The password account exists only for interface testing. Production deployment
will use per-device certificate or key authentication, account expiry, rate
limits, and an audited support-access workflow without changing the command
metadata or JSON API.
