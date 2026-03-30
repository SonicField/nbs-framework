# Session Metadata

Session metadata records the state of a running agent session. It is written by `nbs-claude` when an agent launches, read by `nbs-workers` to display session info and resume sessions, and deleted by `nbs-chat-terminal-restart.sh` on team restart.

This document is the **specification** for the Honest format that replaces the legacy JSON format. Part 2 of the migration implements this specification.

## Honest Type Definition

```pascal
type
  { Transport mode for the agent session.
    Auto: default mode, determined at startup.
    Ts: nbs-ts terminal session mode.
    These are the only valid values — honest-build rejects
    any other value at build time. }
  Transport = (Auto, Ts);

  { Session metadata for a running agent.
    Stored in .nbs/sessions/<handle>.honest as an Honest document.
    Built by honest-build, parsed by hon_parse_file in C.
    All LongInt fields are 64-bit integers internally (int64_t). }
  SessionMeta = record
    session_id         : String;     { UUID, generated at launch }
    handle             : String;     { Agent handle, e.g. "supervisor" }
    model              : String;     { Claude model, e.g. "opus[1m]" }
    nbs_ts_handle      : String;     { nbs-ts session name, empty until ts mode }
    transport          : Transport;  { Auto or Ts }
    started            : String;     { ISO 8601 UTC, e.g. "2026-03-30T14:52:17Z" }
    project_root       : String;     { Absolute path to project directory }
    pid                : LongInt;    { OS process ID, must be > 0 }
    initial_prompt_set : Boolean;    { True if an initial prompt was provided }
  end;
```

### Rulebook

The `SessionMeta` type is defined in its own rulebook file: `~/.nbs/honest/session.honest-rulebook`. This keeps session metadata documents small — no dependency chain from the NBS standard rulebook. The rulebook contains only `SessionMeta`, `Transport`, and their primitive dependencies.

## File Location

```
<project-root>/.nbs/sessions/<handle>.honest
```

Example: `/home/user/project/.nbs/sessions/supervisor.honest`

The directory is `SESSIONS_SUBDIR = ".nbs/sessions"`.

## Building a Session Document

`nbs-claude` builds session metadata using `honest-build`:

```bash
"$NBS_ROOT/.nbs/bin/honest-build" --type SessionMeta \
  --rulebook "$HOME/.nbs/honest/session.honest-rulebook" \
  session_id="$SESSION_UUID" \
  handle="$SIDECAR_HANDLE" \
  model="$NBS_MODEL" \
  nbs_ts_handle="$NBS_TS_HANDLE" \
  transport="$TRANSPORT_VALUE" \
  started="$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
  project_root="$NBS_ROOT" \
  pid=$$ \
  initial_prompt_set="$PROMPT_BOOL" \
  > "$SESSION_META"
```

**Failure mode:** The shell `>` redirect truncates the target file *before* `honest-build` runs. If `honest-build` then fails (invalid enum value, missing field, OOM), the result is an empty file, not an absent one. `worker.c` must handle empty/unparseable `.honest` files gracefully — `hon_parse_file` will return an error, unlike the legacy JSON path where a truncated heredoc could produce partial but readable data.

## Who Writes

**`bin/nbs-claude`** writes the session metadata file twice during agent startup:

### First Write (line ~285)

Immediately after generating the session UUID. All fields are set, but `nbs_ts_handle` is empty and `transport` is `Auto`.

### Second Write (line ~380)

After the nbs-ts session is created (in ts transport mode only). The file is overwritten with the same fields, but now `nbs_ts_handle` contains the actual session name and `transport` is `Ts`.

**Race condition:** If `nbs-workers` reads the file between the two writes, it may see a document with empty `nbs_ts_handle`. This is benign — the worker retries or the user re-runs the command. The Honest format does not change this race; it was present in the JSON format and is inherent to the two-phase write design.

### Field Sources

| Field | Source |
|-------|--------|
| `session_id` | `uuidgen` or `/proc/sys/kernel/random/uuid` |
| `handle` | `$SIDECAR_HANDLE` (default: `"claude"`) |
| `model` | `$NBS_MODEL` (default: `"opus[1m]"`) |
| `nbs_ts_handle` | Set after `nbs-ts` session creation |
| `transport` | `Auto` (default) or `Ts` (after nbs-ts session) |
| `started` | `date -u +%Y-%m-%dT%H:%M:%SZ` |
| `project_root` | `$NBS_ROOT` |
| `pid` | `$$` (shell process ID) |
| `initial_prompt_set` | `True` if `$INITIAL_PROMPT` is non-empty, `False` otherwise |

**Write-only fields:** `handle`, `transport`, and `initial_prompt_set` are written by `nbs-claude` but never read by `worker.c`. They exist for diagnostic inspection (via `honest-get` or `nbs-workers session`) but do not affect runtime behaviour.

## Who Reads

**`src/nbs-workers/worker.c`** reads session metadata using the Honest C API:

```c
#include "honest.h"

hon_diag_list diags = {0};
hon_document *doc = hon_parse_file(meta_file, &diags);

char session_id[128];
char model[128];
if (hon_get_string(doc, "result", "session_id", session_id, sizeof(session_id)) != 0) {
    /* handle error */
}
if (hon_get_string(doc, "result", "model", model, sizeof(model)) != 0) {
    /* handle error */
}
long pid_val;
if (hon_get_long(doc, "result", "pid", &pid_val) != 0) {
    /* handle error */
}
/* ... */
hon_doc_free(doc);  /* NOT free() — honest owns the arena */
```

### `nbs-workers continue <handle>` (line ~1745)

Reads: `session_id`, `model`, `project_root`, `nbs_ts_handle`, `pid`.

Uses the `nbs_ts_handle` to attach to the existing terminal session. Uses `pid` to check if the original process is still alive.

### `nbs-workers session <handle>` (line ~1916)

Reads: `session_id`, `model`, `started`, `project_root`, `nbs_ts_handle`, `pid`.

Displays session information to the user.

### Memory Management

`hon_parse_file` returns a `hon_document*` that must be freed with `hon_doc_free(doc)`, not `free()`. `hon_get_string` copies the field value into a caller-provided buffer — the buffer is independent of the document and remains valid after `hon_doc_free`. If any code path uses `free()` instead of `hon_doc_free`, the result is memory corruption.

## Who Deletes

**`bin/nbs-chat-terminal-restart.sh`** (line 119):

```bash
rm -f "${PROJECT_ROOT}/.nbs/sessions/"*.honest 2>/dev/null || true
```

All session files are deleted on team restart. Session files are ephemeral — they exist only while the agent is running.

Also cleaned on restart:
- PID files: `.nbs/pids/*.pid`
- Control state: `.nbs/control-pause`
- Trigger timestamps: librarian, pythia, shepard, fixup last-run markers

## Extracting Fields from Shell

```bash
honest-get .nbs/sessions/supervisor.honest result session_id
# → a1b2c3d4-e5f6-7890-abcd-ef1234567890

honest-get .nbs/sessions/supervisor.honest result pid
# → 12345
```

## Migration Context

The legacy format stored session metadata as JSON in `.nbs/sessions/<handle>.json`, written via bash heredoc and parsed by `json_extract_string()` / `json_extract_number()` (simple `strstr()`-based parsers in worker.c). The Honest format replaces both the file format and the parsing code. The JSON parsing functions (~90 lines) are deleted as part of the migration.

## See Also

- [Control Files](control-files.md) — PID files and other per-agent state
- [Sidecar Notifications](sidecar.md) — the sidecar that runs alongside each session
