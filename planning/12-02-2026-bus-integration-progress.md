# NBS Bus Integration — Progress

**Date:** 12 February 2026

## Session 1: 12 February 2026

### Plan created
- Read doc-claude's corrected bus documentation (docs/nbs-bus.md, concepts/coordination.md, docs/nbs-bus-recovery.md)
- Read engineering standards
- Read current nbs-chat source (main.c, chat_file.h, Makefile)
- Wrote plan: 12-02-2026-bus-integration-plan.md
- Design decision: shell out to `nbs-bus publish` rather than writing event files directly (single source of truth for format)

### Coordination
- Agreed split with bench-claude: bench-claude takes core nbs-bus binary, I take integration
- Caught two errors in doc-claude's docs (retention: size-based not time-based; all chat generates events, not just @mentions)
- Alex confirmed MVP-first with adversarial tests before expanding

### Implementation complete

**New files:**
- `src/nbs-chat/bus_bridge.h` — Header with full pre/postcondition docs
- `src/nbs-chat/bus_bridge.c` — Implementation: bus_find_events_dir(), bus_extract_mentions(), bus_bridge_after_send()
- `tests/automated/test_nbs_chat_bus.sh` — 20 tests (8 pass now, 12 skip awaiting nbs-bus binary)

**Modified files:**
- `src/nbs-chat/main.c` — Added `#include "bus_bridge.h"`, call to `bus_bridge_after_send()` after successful `chat_send()`
- `src/nbs-chat/Makefile` — Added `bus_bridge.c` to LIB_SRCS, added dependencies, added bus test to test targets, added `-D_DEFAULT_SOURCE` for `realpath()`

### Design decisions
- **Shell out to nbs-bus via fork+exec** (not `system()`) — avoids shell injection, maintains single source of truth for event format
- **Bus failure is non-fatal** — `bus_bridge_after_send()` always returns 0. Tested: read-only bus dir, missing nbs-bus binary, deleted bus dir
- **@mention detection**: `@` preceded by whitespace or start-of-string, followed by `[a-zA-Z0-9_-]+`. Email-like patterns excluded (preceded by alphanumeric/dot/underscore)
- **Event payload**: truncated to 2048 bytes. Full message is in the chat file — the event is a signal, not a report.

### Test results

**Release build:** 63 tests pass (15 lifecycle + 28 terminal + 20 bus)
**ASan build:** 63 tests pass (15 lifecycle + 28 terminal + 20 bus)

Bus-dependent tests (12 of 20) correctly skip when nbs-bus binary is not available.

### Learnings
1. `realpath()` needs `-D_DEFAULT_SOURCE` on glibc when `-std=c11` is used (the standard `-D_POSIX_C_SOURCE=200809L` is not sufficient for this function on some glibc versions)
2. The fork+exec approach for bus_publish is cleaner than system() — no shell escaping needed, payload passed as a single argv element
3. Test isolation is important: each test uses its own project directory under a fresh tmpdir

### Blocking on (RESOLVED)
- ~~bench-claude's `nbs-bus` binary~~ — Available. All 20/20 bus integration tests pass.

## Session 2: 12 February 2026 (continued)

### End-to-end integration verified
- bench-claude completed nbs-bus binary (17 tests, all pass)
- All 20 chat-bus integration tests now pass (previously 12 were skipping)
- Full suite: 63 tests pass under both release and ASan

### Review of bench-claude's nbs-bus binary

doc-claude posted first review, identified 3 issues:
1. bus.h header comment had wrong filename format (milliseconds, no PID) — **fixed**
2. `timestamp_ms` field name should be `timestamp_us` — **fixed**
3. chat-mention priority was "normal", should be "high" — **fixed** (in bus_bridge.c)

My independent review found 5 additional items:

| Issue | Severity | Description |
|-------|----------|-------------|
| A | Design | `bus_event_t.source` and `.type` never populated by `scan_events()` — dead struct fields |
| B | Code quality | `#pragma GCC diagnostic ignored "-Wformat-truncation"` is file-scoped; could mask real truncation bugs in YAML-writing code |
| C | Doc-code gap | Deduplication documented as active behaviour (nbs-bus.md) but not implemented; `dedup-key` written to files but never enforced |
| D | Doc-code gap | `config.yaml` documented but not read; retention configured only via `--max-bytes` CLI flag |
| E | Cosmetic | Feature-test macro inconsistency: nbs-bus uses `_GNU_SOURCE`, nbs-chat uses `_POSIX_C_SOURCE + _DEFAULT_SOURCE` |

Recommendation: ship as MVP. Issues A-B for next iteration. Issues C-D require either implementation or docs update to mark as "planned".

### Learnings
4. Cross-review between agents catches different classes of issues: doc-claude caught naming/spec mismatches, I caught structural design issues and doc-code gaps
5. The dedup-key gap is a good example of docs outrunning implementation — the field is written for forward compatibility but the docs claim enforcement that doesn't exist
