# Plan: nbs-ts Named Sessions

## Problem

nbs-ts sessions have random hex handles (`a2f31411`). This makes:
- **Debugging hard**: `nbs-ts list` shows 100+ sessions with no indication of which team or role they belong to
- **Filtering impossible**: scripts can't find "all sessions for the poem team" without reading JSON metadata files
- **Cleanup fragile**: no way to distinguish team sessions from test sessions, workers, or orphans
- **Fixup broken**: the fixup worker has to read `.nbs/sessions/*.json` instead of just grepping session names

The old tmux approach used `nbs-<handle>-<tag>` naming (e.g. `nbs-supervisor-poem`) which was inherently robust and debuggable.

## Design

### nbs-ts changes

Add `--name=NAME` to `nbs-ts create`. The name is stored in a `name` file in the session directory and displayed in `nbs-ts list`.

```
nbs-ts create --name=nbs-supervisor-poem "claude --model opus..."
```

Add `nbs-ts list --name=PATTERN` to filter by name (substring match).

```
nbs-ts list --name=poem
```

Output format change — add name column:

```
a2f31411    alive    nbs-supervisor-poem    claude --model opus...
f84d2aab    alive    nbs-generalist-poem    claude --model opus...
c1bfe416    alive    nbs-scribe-poem        claude --model opus...
```

Sessions without a name show `-` in the name column.

Also add `nbs-ts find NAME` — returns the handle for a named session (exact match). Exit 0 if found, exit 2 if not. This lets scripts look up sessions by name without parsing list output.

```
$ nbs-ts find nbs-supervisor-poem
a2f31411
```

### Naming convention

`nbs-<handle>-<tag>` where tag comes from the chat filename:
- `poem.chat` → tag `poem` → `nbs-supervisor-poem`
- `live.chat` → tag `live` → `nbs-supervisor-live`
- `nn.Module.chat` → tag `nn-Module` → `nbs-supervisor-nn-Module`

This matches the old tmux convention exactly.

### Files to modify

#### Phase 1: nbs-ts core (test + implement)

1. **src/nbs-ts/main.c** — `cmd_create`:
   - Accept `--name=NAME` flag
   - Write name to `<session_dir>/name` file
   - Validate name: `^[a-zA-Z0-9_.-]+$`, max 64 chars

2. **src/nbs-ts/main.c** — `cmd_list`:
   - Read `name` file for each session
   - Display in output: `<handle>\t<status>\t<name>\t<command>`
   - Accept `--name=PATTERN` flag for substring filtering

3. **src/nbs-ts/main.c** — add `cmd_find`:
   - Scan all sessions for exact name match
   - Print handle on match, exit 0
   - Exit 2 if not found

#### Phase 2: nbs-claude and restart script

4. **bin/nbs-claude** — ts mode:
   - Derive tag from chat filename in NBS_ROOT: `basename $(ls .nbs/chat/*.chat | head -1) .chat | tr '.' '-'`
   - Pass `--name=nbs-${NBS_HANDLE}-${tag}` to `nbs-ts create`

5. **bin/nbs-chat-terminal-restart.sh** — step 1 (kill):
   - Use `nbs-ts list --name=<tag>` to find only this team's sessions
   - Kill only matching sessions, not all sessions globally

6. **bin/nbs-chat-terminal-restart.sh** — step 4 (spawn):
   - No change needed — nbs-claude handles naming

#### Phase 3: sidecar and fixup

7. **src/nbs-sidecar/transport_ts.c** — no change needed (works with handles, not names)

8. **bin/nbs-spawn-worker** — pass `--name=nbs-<role>-worker-<suffix>` to distinguish workers from team sessions

9. **bin/nbs-sidecar-restart** — use `nbs-ts list --name=` to find team sessions instead of scanning all sessions

10. **~/.nbs/commands/nbs-fixup-auto.md** — instruct fixup to use `nbs-ts list --name=<tag>` to find team sessions

#### Phase 4: terminal.c agent count

11. **src/nbs-chat/terminal.c** — the `--restart` safety check can use `nbs-ts list --name=<tag> | grep -c alive` for accurate project-scoped counting (better than pid file scanning)

### What does NOT change

- The hex handle remains the primary identifier for all nbs-ts commands (status, read-new, send, kill, etc.)
- Session directory structure (`~/.nbs-ts/sessions/<hex-handle>/`) unchanged
- Name is optional — sessions without `--name` work exactly as before
- No breaking changes to existing sessions

## Test-Driven Development

Tests are written FIRST, before implementation. Each test must fail before the code is written, and pass after.

### Test file: tests/automated/test_nbs_ts_named_sessions.sh

Tests for Phase 1 (nbs-ts core):

| ID | Test | Command | Expected |
|----|------|---------|----------|
| N1 | Create with name | `nbs-ts create --name=test-alpha bash` | Returns handle, `<session_dir>/name` contains "test-alpha" |
| N2 | Create without name | `nbs-ts create bash` | Returns handle, no `name` file (or `-` in list) |
| N3 | Name validation rejects bad chars | `nbs-ts create --name='bad name!' bash` | Exit non-zero, no session created |
| N4 | Name validation rejects too long | `nbs-ts create --name=$(python3 -c "print('a'*65)") bash` | Exit non-zero |
| N5 | List shows name column | Create 2 named + 1 unnamed, `nbs-ts list` | All 3 appear, named ones show name, unnamed shows `-` |
| N6 | List filters by name | Create `test-alpha` and `test-beta`, `nbs-ts list --name=alpha` | Only `test-alpha` appears |
| N7 | Find exact match | Create `test-alpha`, `nbs-ts find test-alpha` | Prints handle, exit 0 |
| N8 | Find no match | `nbs-ts find nonexistent` | Exit 2, no output |
| N9 | Find partial no match | Create `test-alpha`, `nbs-ts find test` | Exit 2 (find is exact, not substring) |
| N10 | Name survives session death | Create named session with `exit 0`, wait, check name in list | Name still shown for dead session |

### Test file: tests/automated/test_nbs_ts_named_integration.sh

Tests for Phase 2-4 (integration):

| ID | Test | What to verify |
|----|------|---------------|
| I1 | nbs-claude creates named session | Spawn nbs-claude with NBS_HANDLE=supervisor in a temp project with poem.chat, verify `nbs-ts list` shows `nbs-supervisor-poem` |
| I2 | Restart kills only its team | Create sessions named `nbs-X-poem` and `nbs-X-other`, run restart for poem, verify poem sessions killed and other sessions untouched |
| I3 | Spawn worker creates named session | Run nbs-spawn-worker, verify session has `nbs-<role>-worker-<suffix>` name |
| I4 | Restart agent count scoped | Create named sessions for two teams, verify terminal.c count only counts the current team |

## Execution Plan — 4 Sub-Agents

### Agent 1: nbs-ts core (Phase 1)

**Input:** This plan, src/nbs-ts/main.c

**Tasks:**
1. Write `tests/automated/test_nbs_ts_named_sessions.sh` (tests N1-N10)
2. Run tests — verify all fail (no implementation yet)
3. Implement `--name` in `cmd_create` (write name file, validate)
4. Implement name column in `cmd_list` + `--name=PATTERN` filter
5. Implement `cmd_find`
6. Update `cmd_help` and `cmd_clean` (clean should also remove name file)
7. Rebuild, run tests — verify all pass
8. Run existing nbs-ts test suite — verify no regressions

### Agent 2: nbs-claude + restart (Phase 2)

**Depends on:** Agent 1 complete (needs working `--name` flag)

**Input:** This plan, bin/nbs-claude, bin/nbs-chat-terminal-restart.sh

**Tasks:**
1. Write `tests/automated/test_nbs_ts_named_integration.sh` (tests I1-I2)
2. Modify bin/nbs-claude ts mode — derive tag, pass `--name=nbs-${HANDLE}-${tag}`
3. Modify bin/nbs-chat-terminal-restart.sh — use `nbs-ts list --name=<tag>` for kill step
4. Run integration tests — verify I1-I2 pass
5. Run existing restart test — verify no regression

### Agent 3: spawn-worker + sidecar-restart (Phase 3)

**Depends on:** Agent 1 complete

**Input:** This plan, bin/nbs-spawn-worker, bin/nbs-sidecar-restart

**Tasks:**
1. Write test I3 (worker naming)
2. Modify bin/nbs-spawn-worker — pass `--name=nbs-<role>-worker-<suffix>` to `nbs-ts create`
3. Modify bin/nbs-sidecar-restart — use `nbs-ts list --name=` instead of scanning all sessions
4. Update `~/.nbs/commands/nbs-fixup-auto.md` — instruct to use `nbs-ts list --name=<tag>`
5. Run test I3 — verify pass

### Agent 4: terminal.c + final integration (Phase 4)

**Depends on:** Agents 1-3 complete

**Input:** This plan, src/nbs-chat/terminal.c

**Tasks:**
1. Write test I4 (scoped agent count)
2. Modify terminal.c `--restart` safety check — use `nbs-ts list --name=<tag> | grep -c alive`
3. Rebuild nbs-chat-terminal
4. Run test I4 — verify pass
5. Run full test suite (`tests/run_all.sh`) — verify no regressions
6. Run `test_nbs_ts_no_tmux.sh` — verify Phase 6 still passes

## Sequencing

```
Agent 1 (core)  ──────────────────┐
                                  ├──→ Agent 4 (terminal + final)
Agent 2 (nbs-claude + restart) ───┤
Agent 3 (worker + sidecar) ───────┘
```

Agents 2 and 3 can run in parallel after Agent 1 completes. Agent 4 runs last as the integration gate.

## Verification

1. `nbs-ts create --name=test-session bash` → session dir contains `name` file with "test-session"
2. `nbs-ts list` → shows name column
3. `nbs-ts list --name=test` → filters to matching sessions only
4. `nbs-ts find test-session` → returns hex handle
5. `nbs-ts find nonexistent` → exit 2
6. Start a team via restart script → all sessions have `nbs-<handle>-<tag>` names
7. `nbs-ts list --name=poem` → shows only poem team sessions
8. Restart script kills only poem sessions, not other teams or test sessions
9. Fixup worker finds and reports only its team's agents

## Falsifier

If after implementation, `nbs-ts list --name=poem` returns sessions from a different team, or misses sessions from the poem team, the naming is broken.
