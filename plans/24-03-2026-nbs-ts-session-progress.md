# Progress Log: nbs-ts Session (24 March 2026)

## Terminal Goal

Complete the nbs-ts migration: make nbs-ts the sole session management system, remove all tmux dependencies, and get a team running reliably.

## What was done

### Phase 1: Bug fixes from test suite (commits 1adfbf0 → 0694882)

| Fix | Root cause | How found |
|-----|-----------|-----------|
| nbs-spawn-worker stdout leak | Background monitor subshell inherited stdout, blocking `$()` callers | test_nbs_ts_worker hung |
| Last tmux reference in terminal.c | `tmux list-sessions` in restart safety check | grep scan |
| Oneshot exit code through helper | `exit 42` terminates before trailing commands; `waitpid` fails on helper-spawned children | test_nbs_ts_oneshot O3 failure |
| Recursive test glob in test_nbs_ts_no_tmux | NT2 included itself via `test_nbs_ts_*.sh` glob | Test timeout |
| Removed src/nbs-pty-session/ artefacts | Stale build directory from pre-migration | NT1b check |

**Exit code fix detail:** Non-interactive commands via helper now use `trap EXIT` to write exit code to completion.log. The daemon reads completion.log when `waitpid` fails (helper-spawned children aren't its own).

### Phase 2: Restart script fixes (commits 3f7a5bc → 5a16456)

| Fix | Root cause | How found |
|-----|-----------|-----------|
| Double-session bug | Restart script wrapped nbs-claude in nbs-ts create; nbs-claude created its own session internally | Agent trust dialog showed wrong workspace |
| Working directory | Helper spawns bash in HOME; nbs-claude needed --root and explicit cd | Claude opened in /home/alexturner not project dir |
| Trust dialog false positive | `detect_prompt_visible` detected `❯` in trust dialog, consumed initial prompt | Agents sat idle after startup |
| Skill injection via file | Full skill files (5-11KB) exceeded env var limits when passed via NBS_INITIAL_PROMPT | Agents died immediately after spawn |
| setsid for process survival | Background subshells received SIGHUP on parent exit; disown doesn't work in non-interactive bash | Agents died when restart script exited |
| Cleanup race condition | Old agents' cleanup traps fired after new agents started, deleting new sessions | Stale sessions after restart |
| Prompt detection scope | "bypass" requirement in detect_prompt_visible broke all notification injection when agents were mid-work | Sidecars couldn't inject after restart |

### Phase 3: @mention? improvements (commits 0694882 → b12e92d)

- **Cursor-right → spaces**: CSI `<n>C` sequences replaced with spaces in strip_ansi (Claude uses cursor positioning instead of literal spaces)
- **Blank/spinner filtering**: Lines with < 4 alphanumeric chars stripped (removes thinking animation debris)
- **Full buffer capture**: Changed from 128 lines to full 32KB buffer (Claude's 5:1 junk ratio meant 128 lines was seconds of animation)
- **16-line output**: Reduced from 32 to 16 non-blank lines
- **Query priority**: Moved @mention? check before interrupt/mention checks in sidecar main loop (queries were delayed by sleep(3) in interrupt handler)

### Phase 4: Named sessions (commits aa6bf61 → 4b02336)

4 sub-agents executed in parallel:
1. **Core**: `--name=NAME` on create, name column in list, `--name=PATTERN` filter, `find` command. 10 tests (N1-N10).
2. **nbs-claude + restart**: auto-derives `nbs-<handle>-<tag>` names, restart scopes kills by `--name=<tag>`. Integration tests I1-I2.
3. **Workers**: nbs-spawn-worker passes `--name=nbs-<role>-worker-<suffix>`, nbs-workers C code uses `nbs-ts find` for name-based lookups. Test I3.
4. **terminal.c**: restart safety check uses `nbs-ts list --name=<tag>` for project-scoped counting.

### Phase 5: Documentation sweep

2 sub-agents in parallel:
- **Docs**: 9 files updated, `pty-session.md` deleted
- **Skills**: 8 files updated, `nbs-tmux.md` and `nbs-tmux-worker.md` deleted

### Phase 6: Miscellaneous

- Skill files updated for nbs-ts: nbs-fixup-auto.md, nbs-teams-fixup.md, nbs-teams-restart.md, nbs-teams-start.md, nbs-teams-help.md
- nbs-teams-start.md rewritten to call nbs-chat-init instead of reimplementing infrastructure setup
- Digest banner removed (posted before digest completed)
- Digest task description simplified (removed slash command dependency)
- Agent count in terminal.c scoped to project (was global nbs-ts list)

### Phase 7: Skill file rewrites (commit 36f80cf)

All 6 team skill files rewritten. Added "How you receive work" section near the top of each, explaining the `[NBS-CHAT-NOTIFICATION]` mechanism and explicitly forbidding `sleep`, timers, and polling loops. Teams-chat trimmed from 253 to 142 lines. Root cause: agents were creating `sleep 300` polling loops because the "do not poll" instruction was buried at line 200+.

### Phase 8: Scorched earth init (commits 4482a89 → b05ab80)

`nbs-chat-init` Phase 0 now destroys all state from previous runs: kills nbs-ts sessions (by name), kills nbs-claude wrappers (by pid), kills sidecars (by pkill), waits for death, force-kills survivors, then removes control-pause, registries, pids, sessions, trigger timestamps, worker files, bus events (pending and processed), sidecar logs, scribe logs, archives, digests, and locks.

Root cause: stale `control-pause` file from a previous `/pause` survived across restarts and silently disabled all sidecars.

### Phase 9: Detection refactor (commit 2dd5677)

Split `detect_prompt_visible` into three functions:
- `detect_prompt_idle` — ❯ visible (notifications)
- `detect_prompt_ready` — ❯ OR interrupted prompt (interrupt handler)
- `detect_prompt_not_trust` — ❯ AND NOT trust dialog (init-wait)

Root cause: one function serving five callers with different requirements. Every hardening change for one caller broke another.

### Phase 10: Spawn consolidation (commit b9bc5e9)

Deleted `nbs-spawn-worker` (bash). `nbs-workers spawn` (C) is now the single entry point for worker lifecycle. Added `--skill=FILE` to `nbs-workers spawn`. Updated `triggers.c` and `terminal.c` to call `nbs-workers spawn` instead.

### Phase 11: Sidecar auto-restart fix (commit 710434a)

Replaced `disown` with `setsid` + temp script for the sidecar auto-restart loop. `disown` doesn't work in non-interactive bash (which is how the restart script spawns agents). The sidecar loop died on first signal with no recovery.

### Phase 12: Miscellaneous fixes

- `nbs-ts gc` command — garbage collect dead sessions (cleaned 3500+ orphans)
- Removed digest banner (posted before digest completed)
- Fixed pythia-interval default (20 → 10, matching nbs-chat-init)
- Removed hardcoded 'Alex' from restart messages and docs
- Fixed spawn order: scribe → supervisor → gatekeeper → theologian → testkeeper → generalist
- Non-blocking sidecar init (queries work from first tick)
- 30-line capture window for prompt detection (was 5)
- Interrupted prompt detection ("What should Claude do")
- tools.md updated with full nbs-ts command reference
- nbs-teams-start.md rewritten to call nbs-chat-init
- nbs-fixup-auto.md and nbs-teams-fixup.md updated for nbs-ts
- new-goal.md written for poem team's next phase

## What failed

- **Overcorrection cascade**: Adding "bypass" to detect_prompt_visible broke notification injection everywhere. Should have been a local guard in init-wait only.
- **ESC interrupt delivery**: Supervisor got stuck, `@supervisor!` didn't land. Root cause never established — stopped investigating when Alex pointed out I was speculating.
- **Sidecar auto-restart**: Sidecars die and don't come back. The auto-restart loop in nbs-claude doesn't survive external kills. Manual respawning needed 3 times.
- **Orphan session accumulation**: 3500+ zombie sessions from testing. No gc until end of session.

## What was learned

- **Named sessions should have been in the original nbs-ts design.** Random hex handles made everything harder — debugging, filtering, cleanup, fixup.
- **Slash commands through sidecar injection are unreliable.** File references work. Plain text works. Slash commands depend on client-side expansion timing.
- **The restart path needs end-to-end testing.** Every fix was discovered in production by Alex.
- **`setsid` not `disown`** for surviving parent exit in non-interactive bash.
- **Claude's terminal output is ~5:1 junk.** Cursor movements, thinking animations, blank lines. Any feature that reads terminal output needs aggressive filtering.

## Commits (chronological)

1. `1adfbf0` Fix tmux removal blockers
2. `3f7a5bc` Fix restart double-session bug
3. `0694882` Fix @name? query — strip blank lines
4. `0233132` Scope restart agent count to project
5. `be16324` Filter thinking animation from @name? output
6. `f3f097d` Cursor-right → spaces in strip_ansi
7. `5991f0b` Fix prompt detection false positive on trust dialog
8. `75ed4da` Inject role prompts as plain text, not slash commands
9. `31a8101` Use paste brackets for initial prompt injection
10. `c343a51` Fix restart cleanup race
11. `65fa548` Disown spawned agents (later replaced by setsid)
12. `5a16456` Pass skill content via file reference
13. `3d5623e` Full buffer capture + better filtering for @mention?
14. `0cbc45d` Reduce @mention? to 16 lines
15. `b12e92d` Query priority before interrupts/mentions
16. `6ed799d` Revert bypass check, guard trust dialog locally
17. `aa6bf61` Named sessions Phase 1 (core)
18. `63d43d7` Named sessions Phase 2 (nbs-claude + restart)
19. `604c05a` Named sessions Phase 3 (workers)
20. `0ce766f` Named sessions Phase 4 (terminal.c + sidecar)
21. `ff50a46` Docs updated for nbs-ts
22. `4b02336` Plan + miscellaneous
23. `36f80cf` Rewrite all 6 team skill files — fix notification model
24. `2ccd993` Remove digest banner, simplify task, add progress log
25. `a01e714` Add nbs-ts gc — garbage collect dead sessions
26. `1ad817b` Update tools.md — full nbs-ts command reference
27. `41dce29` Redesign sidecar startup — no blocking init-wait
28. `46aaf16` Fix pythia-interval default (20 → 10)
29. `cfa38a2` Fix spawn order, remove hardcoded 'Alex'
30. `51b9ba7` Clear control-pause on restart
31. `4482a89` Phase 0 scorched earth cleanup in nbs-chat-init
32. `47d1ddd` Phase 0: kill processes before clearing files
33. `0a3d66a` Phase 0: destroy scribe logs, archives, digests
34. `b05ab80` Phase 0: destroy processed bus events
35. `c30d78b` Detect Claude's interrupted prompt state
36. `2dd5677` Refactor: split detect_prompt_visible into three functions
37. `b9bc5e9` Delete nbs-spawn-worker — consolidate to nbs-workers spawn
38. `710434a` Fix sidecar auto-restart — setsid instead of disown
39. `5a63085` Plan: nbs-claude --daemon
