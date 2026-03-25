# Chapter 7: Troubleshooting

Symptom, diagnosis, fix. Find your problem, follow the steps.

## Agent not responding to chat messages

**Symptom:** You send messages but an agent never replies.

**Check 1: Is the agent running?**

```bash
nbs-workers list
```

If the agent's session has died, her status will show `died`. Resume her with:

```bash
nbs-workers continue <name>
```

This respawns a fresh session using the saved session ID (`claude --resume`), so she picks up where she left off. Alternatively, restart the whole team with `/restart`.

**Check 2: Is the sidecar polling?**

The sidecar injects notifications only when the agent is idle at a prompt. If the agent is busy with a long task, she will not check chat until the task completes.

**Check 3: Is the agent watching the right chat file?**

```bash
cat .nbs/control-registry-<handle>
```

The chat file should appear as `chat:<path>`. If missing, the agent does not know about it. Register it:

```bash
echo "register-chat .nbs/chat/c11-interp.chat" >> .nbs/control-inbox-<handle>
```

**Check 4: Is the @mention correct?**

Mentions must match the agent's handle exactly. Check the agent's handle by looking at its previous chat messages or its worker task file.

## Events not being processed

**Symptom:** `nbs-bus check .nbs/events/` shows pending events but no one processes them.

**Check 1: Is any agent polling the bus?**

```bash
cat .nbs/control-registry-* | grep bus
```

If no agent has the bus registered, no one is checking it.

**Check 2: Are events being acknowledged?**

Events stay pending until `nbs-bus ack` moves them to `processed/`. If agents read but do not ack, events accumulate.

**Check 3: Is deduplication dropping events?**

```bash
nbs-bus publish .nbs/events/ test test-event normal --dedup-window=0 "test"
nbs-bus check .nbs/events/
```

If the event appears, your real events may be getting deduplicated. Check `dedup-window` in `.nbs/events/config.yaml`.

## Cannot see messages from another agent

**Symptom:** Agent A posts messages but Agent B never sees them.

**Check 1: Same file path?** Both agents must read/write the exact same file path. Relative vs absolute path differences can cause this.

**Check 2: Using --unread correctly?** Use `--unread=<handle>` for polling, not `--since=<handle>`. The `--since` option shows messages since your last *post*, not your last *read*.

## Sidecar not injecting notifications

**Symptom:** The agent sits idle but never checks for messages or events.

**Check 1: Is the agent running via nbs-claude?** If you launched Claude Code directly (not via `nbs-claude`), there is no sidecar.

**Check 2: Is the sidecar process alive?**

```bash
ps aux | grep nbs-sidecar
```

If the sidecar crashed, restart the agent.

**Check 3: Prompt detection.** The sidecar only injects when it sees a prompt character in the last 3 lines of session output. If the agent is in a state without a visible prompt, the sidecar will not inject.

## Workers stuck in plan mode

**Symptom:** Claude Code asks "Would you like to proceed?" and the worker blocks.

The sidecar auto-selects option 2 ("Yes, and bypass permissions"). If this is not happening:

**Check 1: Is the sidecar running?** (See above)

**Check 2:** The sidecar matches the exact string "Would you like to proceed?" in session output. If Claude Code changes the prompt wording, detection breaks.

## Bus directory missing

**Symptom:** `nbs-bus` commands fail with "Events directory not found."

**Fix:**

```bash
mkdir -p .nbs/events/processed
```

## Scribe not recording decisions

**Symptom:** `.nbs/scribe/live-log.md` is empty or stale despite active chat.

**Check 1: Is the Scribe instance running?** Look for an active Scribe session.

**Check 2: Is the log file writable?**

```bash
touch .nbs/scribe/live-log.md
```

**Check 3: Is Scribe polling the correct chat?** The log filename derives from the chat filename: `c11-interp.chat` produces `live-log.md`.

**Impact:** Without Scribe entries, Pythia checkpoints based on decision count are never triggered. The Librarian has no decision log to search.

## Pythia never triggered

**Symptom:** Decisions accumulate but no Pythia assessment appears.

**Check 1: Is the wall-clock trigger enabled?**

```bash
echo $NBS_PYTHIA_INTERVAL
```

If set to 0, the wall-clock trigger is disabled. Check if the decision-count trigger is working:

**Check 2: Has the decision count reached the threshold?**

```bash
grep -c "^### D-" .nbs/scribe/live-log.md
```

Compare against `pythia-interval` in `.nbs/events/config.yaml` (default: 20).

**Check 3: Is the Scribe publishing checkpoint events?**

```bash
ls .nbs/events/*pythia-checkpoint* 2>/dev/null
ls .nbs/events/processed/*pythia-checkpoint* 2>/dev/null
```

## Processed events consuming too much disk space

**Fix:**

```bash
nbs-bus prune .nbs/events/ --max-bytes=16777216
```

This deletes the oldest processed events until the `processed/` directory is under 16MB.

## Multiple oracle spawns at the same time

**Symptom:** Two or three Librarian (or Pythia, Shepard, Fixup) posts appear within seconds of each other.

This can happen when multiple sidecars cross the timer threshold simultaneously. The lock file prevents concurrent spawns, but there is a small window where sequential duplicate spawns can occur.

**This is harmless.** All oracle workers are idempotent. Duplicate runs waste tokens but do not cause incorrect behaviour. If it happens frequently, the re-check after lock acquisition (a 30-second guard) prevents most duplicates.

## All agents disconnected or misconfigured

**Nuclear option:**

```bash
# Remove all NBS state (does not affect your project files)
rm -rf .nbs/

# Recreate
mkdir -p .nbs/chat .nbs/events/processed .nbs/workers .nbs/scribe
nbs-chat create .nbs/chat/c11-interp.chat

# Restart the team
nbs-chat-terminal .nbs/chat/c11-interp.chat <your-handle> --goal-file=goal.md --restart
```

This destroys chat history, event queue, decision log, and worker files. Use it only when you cannot diagnose the problem.

## All agents on same OS user

A hard constraint: all agents must run as the same OS user. `flock` on chat files uses owner-only permissions. If agents run as different users:

- `flock` acquisition silently fails -- concurrent writes may corrupt the chat file
- Cursor tracking diverges -- agents see repeated or missing messages
- No error is reported. The system appears to work but produces incorrect results.

Ensure all agents, including remote agents via `nbs-chat-remote`, connect as the same user.

## Getting Help

If you are stuck and this chapter does not cover your problem:

1. Run `/nbs-teams-help` inside Claude Code for interactive guidance
2. Check `nbs-bus status .nbs/events/` for a summary of the event queue state
3. Read the last 50 chat messages: `nbs-chat export .nbs/chat/c11-interp.chat --last=50 | less -R`
4. Check worker logs: `less .nbs/workers/<name>.log`

## Quick Reference: Diagnosis Commands

```bash
# What workers exist and their status
nbs-workers list

# Bus health
nbs-bus status .nbs/events/

# Pending events
nbs-bus check .nbs/events/

# Chat participants and message counts
nbs-chat participants .nbs/chat/c11-interp.chat

# Last 20 messages
nbs-chat read .nbs/chat/c11-interp.chat --last=20

# Search chat history
nbs-chat search .nbs/chat/c11-interp.chat "error"

# Decision count
grep -c "^### D-" .nbs/scribe/live-log.md

# What each agent is watching
cat .nbs/control-registry-*
```
