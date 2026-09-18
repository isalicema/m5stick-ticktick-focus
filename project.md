# M5StickS3 TickTick Focus Bridge project log

## Current status

- **Current goal:** selectively add public-safe Bridge residency and lifecycle
  observability.
- **Effective decisions:** retain the public repository as a standalone Focus
  Bridge project; do not import private configuration, runtime state, device
  identifiers, local paths, logs, or unrelated reliability changes.
- **Baseline:** local `main` and `origin/main` both resolve to
  `09c616ca7742ead792f249115be73846483af139` at the start of this work.
- **Latest local candidate:** one local commit atop the recorded public baseline;
  use `git log -1` for its immutable ID.
- **Delivery state:** implementation, independent local validation, and the
  approved local-only commit are complete. No push, tag, release, or deployment
  was performed or authorized.
- **Evidence entry points:** `README.md`, `mac-bridge/`,
  `mac-bridge/test_focus_bridge.py`, and this log.

## Records

### P-001 — 2026-09-19 CST (UTC+8) — residency and lifecycle-observability handoff

**Scope:** public Focus Bridge Python process behavior, LaunchAgent template,
tests, documentation, and project log.

- **Alice asked:** selectively synchronize JSON lifecycle logging, signal-aware
  exits, immediate output flushing, and launchd residency settings from a
  separately maintained implementation; create a local commit only.
- **Xingzi's assessment:** the public repository is the correct codebase and
  starts clean at the recorded baseline. The incoming work must be transferred
  hunk by hunk, excluding unrelated TickTick state, timeout, and UI automation
  changes.
- **Actual change:** added immediately flushed JSON lifecycle records for bridge
  startup, startup failure, receipt of supported shutdown signals, runtime
  failure, and stop reasons. HTTP request logging now flushes immediately.
  The generic LaunchAgent template invokes Python with `-u`, uses
  `KeepAlive=true`, and has a ten-second throttle. Added focused lifecycle
  logging and signal tests, and documented the stdout/stderr behavior.
- **Why:** make a long-running public Bridge observable and restartable without
  embedding any local installation details or migrating unrelated state,
  timeout, or UI automation changes.
- **Verification:** `python3 mac-bridge/test_focus_bridge.py` passed 15
  tests; `py_compile` passed with bytecode directed to a temporary cache;
  `plutil -lint` passed; `git diff --check` passed; the added diff had no
  local/private-path markers. A sandboxed start attempt was prevented from
  binding a TCP socket and correctly emitted `bridge_start_failed`; actual
  launchd restart, a real signal-delivery run, Focus-session continuity, and
  device behavior remain unverified in this public checkout.
- **Commit record:** the approved five public files were committed locally by
  Alice with Codex co-authorship; see the latest Git commit for the immutable
  ID and file manifest. The commit is intentionally ahead of `origin/main`.
- **Delivery matrix:** audit, implementation, focused tests, compilation, plist
  lint, diff check, sensitive-data scan, and local commit complete; no push,
  release, deployment, device observation, or Alice acceptance.
