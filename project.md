# M5StickS3 TickTick Focus Bridge project log

## Current status

- **Current goal:** selectively add public-safe Bridge residency and lifecycle
  observability.
- **Effective decisions:** retain the public repository as a standalone Focus
  Bridge project; do not import private configuration, runtime state, device
  identifiers, local paths, logs, or unrelated reliability changes.
- **Baseline:** local `main` and `origin/main` both resolve to
  `09c616ca7742ead792f249115be73846483af139` at the start of this work.
- **Published code:** `a2c4097284bf4a9d6b30011e984c9d54bf781eaa` on public
  `main`; subsequent documentation commits do not change the code candidate.
- **Delivery state:** implementation and validation are complete. Alice authorized
  publishing the existing candidate on 2026-10-15; commit `a2c4097` is now
  pushed to public `main` with remote SHA readback. No tag, release, or
  deployment was performed. Alice also authorized committing and pushing this
  publication log; use Git history and remote main to verify its delivery.
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


### P-002 — 2026-10-15 Asia/Shanghai (UTC+8) — publish existing candidate

- **Alice asked:** inspect the September 19 five-file residency/lifecycle commit
  and push it directly following the prior privacy review.
- **Actual action:** reviewed the exact diff and clean worktree; fetched origin,
  confirmed main was ahead by exactly one commit, verified native Git push
  permission, and pushed the existing commit without changing its contents.
- **Why:** the candidate matched the authorized public scope; no private paths,
  credentials, or concrete IP addresses were found in the added content.
- **Verification:** 15 tests passed; Python compilation, plist lint, and commit
  whitespace checks passed. Remote main independently read back as
  `a2c4097284bf4a9d6b30011e984c9d54bf781eaa`.
- **Delivery matrix:** original commit and push complete; this log update remains
  local and uncommitted. No release, installation, service reload, device
  observation, or runtime acceptance performed. Actual launchd restart, signal
  delivery, and Focus-session continuity remain unverified.


### P-003 — 2026-10-15 Asia/Shanghai (UTC+8) — publication log delivery

- **Alice asked:** commit and push the `project.md` publication log update.
- **Actual change:** reviewed the documentation-only diff, updated the current
  status to name the published code commit, and retained P-002 as the historical
  record of when the log was still local and uncommitted.
- **Why:** publish the delivery evidence alongside the code without modifying
  the implementation or rewriting the original commit.
- **Verification:** working tree contained only `project.md`; whitespace checks
  passed, origin was fetched, and the native Git permission probe succeeded.
- **Delivery evidence:** this documentation commit contains only `project.md`
  and is authorized for push to main. Its commit ID and final remote delivery
  are verifiable through Git history and remote main. No runtime or device
  checks were added in this documentation round.
